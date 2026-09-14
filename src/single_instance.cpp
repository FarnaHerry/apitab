// single_instance.cpp — apitab GUI 进程级单实例锁实现。
#include "single_instance.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace apitab {

SingleInstance::SingleInstance(std::string_view application_id) {
    std::wstring mutex_name = L"Local\\";
    mutex_name.append(application_id.begin(), application_id.end());
    handle_ = ::CreateMutexW(nullptr, TRUE, mutex_name.c_str());
    if (handle_ == nullptr) {
        return;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        state_ = SingleInstanceState::AlreadyRunning;
        return;
    }
    state_ = SingleInstanceState::Acquired;
}

SingleInstance::~SingleInstance() {
    if (handle_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
}

} // namespace apitab

#else

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>

namespace apitab {
namespace {

std::string LockPath(std::string_view application_id) {
    const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
    std::string path = runtime_directory != nullptr && *runtime_directory != '\0' ? runtime_directory : "/tmp";
    if (path.back() != '/') {
        path.push_back('/');
    }
    path.append(application_id.begin(), application_id.end());
    if (runtime_directory == nullptr || *runtime_directory == '\0') {
        path.append("-");
        path.append(std::to_string(static_cast<unsigned long>(::getuid())));
    }
    path.append(".lock");
    return path;
}

} // namespace

SingleInstance::SingleInstance(std::string_view application_id) {
    const std::string path = LockPath(application_id);
    lock_fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd_ < 0) {
        return;
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) {
        state_ = SingleInstanceState::Acquired;
        return;
    }
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
        state_ = SingleInstanceState::AlreadyRunning;
    }
    ::close(lock_fd_);
    lock_fd_ = -1;
}

SingleInstance::~SingleInstance() {
    if (lock_fd_ >= 0) {
        ::flock(lock_fd_, LOCK_UN);
        ::close(lock_fd_);
    }
}

} // namespace apitab

#endif
