// single_instance.h — apitab GUI 进程级单实例锁。
#pragma once

#include <string_view>

namespace apitab {

enum class SingleInstanceState {
    Acquired,
    AlreadyRunning,
    Unavailable,
};

class SingleInstance final {
public:
    explicit SingleInstance(std::string_view application_id);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;
    SingleInstance(SingleInstance&&) = delete;
    SingleInstance& operator=(SingleInstance&&) = delete;

    [[nodiscard]] SingleInstanceState state() const noexcept { return state_; }
    [[nodiscard]] bool acquired() const noexcept { return state_ == SingleInstanceState::Acquired; }
    [[nodiscard]] bool alreadyRunning() const noexcept { return state_ == SingleInstanceState::AlreadyRunning; }

private:
    SingleInstanceState state_ = SingleInstanceState::Unavailable;
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int lock_fd_ = -1;
#endif
};

} // namespace apitab
