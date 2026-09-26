// control_client.cpp — `apitab --cli …` 的客户端半边（见 control.h）。
//
// 这个进程**不构造 store、不碰数据库、不进事件循环**：它只读端点文件、把 argv 发给
// 运行中的实例、把回传的 stdout/stderr 原样回放并沿用它的退出码。命令实现在实例里
// （src/cli.cpp 同一份），所以输出契约与 "--cli 在实例内直跑" 完全一致。
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2 必须在 windows.h 之前：本 TU 直接用 setsockopt/SO_RCVTIMEO 做客户端超时。
#include <winsock2.h>
#include <windows.h>  // CreateProcessW / STARTUPINFOW / MAX_PATH / DETACHED_PROCESS
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
extern char** environ;
#endif

#include "control.h"

// MSVC 例外：`import asio;` 在该工具链下会让本 TU 走文本包含路径并与模块配置冲突
// （CI windows-x86_64 实证：asio/io_context.hpp 的 service 基类消失）。与
// src/ui/tcp_session.cpp 同一取舍——那里也是 MSVC 退回纯头文件，实现仍在 asio 库里
// （ASIO_SEPARATE_COMPILATION，由 asio 目标 PUBLIC 定义）。
#ifdef _MSC_VER
#include <asio.hpp>
#else
import asio;
#endif
import nlohmann.json;
import apitab.config;

namespace apitab::control {

// 当前可执行文件路径：--cli 进程自己就是 apitab，拉起实例 = 再跑一次自己。
std::string ExecutablePath() {
#ifdef _WIN32
    char buffer[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    return n == 0 ? std::string{} : std::string(buffer, n);
#else
    char buffer[4096]{};
    const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return {};
    return std::string(buffer, static_cast<std::size_t>(n));
#endif
}

namespace {

using json = nlohmann::json;

// 拉起后等端点就绪的上限：冷启动要读库、建资源包缓存，给足 30s；超时就报错退出，
// 不无限等（agent 侧需要确定的失败语义）。
constexpr auto kStartupTimeout = std::chrono::seconds{30};
// 就绪探测（ping）要快：实例没起来时不要让客户端干等。
constexpr int kProbeTimeoutMs = 1500;
// 真正命令的兜底：服务端自己有 60s 命令超时，这里留一点余量。
constexpr int kForwardTimeoutMs = 90000;
constexpr auto kStartupPollInterval = std::chrono::milliseconds{200};

// 拉起实例：同一可执行文件、不带 --cli（子进程走 GUI 路径；它会自己拿单实例锁，
// 与这里并发多次拉起天然串行）。
bool SpawnInstance(std::string& error) {
#ifdef _WIN32
    std::wstring command = std::filesystem::path{ExecutablePath()}.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    // DETACHED_PROCESS：不继承调用者的控制台/标准句柄（同 POSIX 侧的理由）。
    const BOOL ok = ::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                                     &startup, &info);
    if (!ok) {
        error = "拉起 apitab 失败（CreateProcess）";
        return false;
    }
    ::CloseHandle(info.hThread);
    ::CloseHandle(info.hProcess);
    return true;
#else
    const std::string exe = ExecutablePath();
    std::vector<std::string> storage{exe};
    std::vector<char*> argv;
    for (std::string& item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    // 子进程必须与**调用者的标准流断开**：被拉起的实例是长驻进程，若它继承了
    // CLI 的 stdout/stderr 管道，`out=$(apitab --cli --ensure …)`、`| cat` 这类
    // 用法会一直等管道关闭（= 等实例退出）——对 agent 来说就是"命令挂住"。
    // 日志本来就走 <dataDir>/logs，不需要 stdout。
    struct FileActions {
        posix_spawn_file_actions_t actions;
        FileActions() { ::posix_spawn_file_actions_init(&actions); }
        ~FileActions() { ::posix_spawn_file_actions_destroy(&actions); }
        FileActions(const FileActions&) = delete;
        FileActions& operator=(const FileActions&) = delete;
    } file_actions;
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        ::posix_spawn_file_actions_addopen(&file_actions.actions, fd, "/dev/null", O_RDWR, 0);
    }

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), &file_actions.actions, nullptr, argv.data(),
                                 environ);
    if (rc != 0) {
        error = "拉起 apitab 失败: " + std::string{std::strerror(rc)};
        return false;
    }
    return true;
#endif
}

// 一次完整的请求/响应往返（同步）。timeoutMs 是套接字读写兜底，避免实例卡住时
// 客户端无限等待。成功解析出 JSON 才算 true。
bool RoundTrip(int port, const std::string& token, const std::vector<std::string>& args,
               json& response, std::string& error, int timeoutMs) {
    asio::io_context context;
    asio::ip::tcp::socket socket{context};
    asio::error_code ec;
    const asio::ip::tcp::endpoint endpoint{
        asio::ip::make_address("127.0.0.1", ec), static_cast<unsigned short>(port)};
    if (ec) {
        error = std::string{"控制面端点无效: "} + ec.message();
        return false;
    }
    socket.connect(endpoint, ec);
    if (ec) {
        error = std::string{"连接运行中的 apitab 失败（127.0.0.1:"} + std::to_string(port) +
                "）: " + ec.message();
        return false;
    }
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    const timeval timeout{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif

    const std::string payload = json{{"args", args}}.dump();
    std::string request = "POST /v1/command HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Authorization: Bearer " + token + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += payload;
    asio::write(socket, asio::buffer(request), ec);
    if (ec) {
        error = std::string{"发送命令失败: "} + ec.message();
        return false;
    }

    // 服务端 Connection: close，所以读到 EOF 即整条响应。手工缓冲读取：
    // 本构建定义 ASIO_NO_IOSTREAM，asio::streambuf/read_until 不可用。
    std::string response_text;
    char chunk[4096];
    for (;;) {
        const std::size_t n = socket.read_some(asio::buffer(chunk), ec);
        if (ec == asio::error::eof) break;
        if (ec) {
            error = std::string{"读取响应失败: "} + ec.message();
            return false;
        }
        response_text.append(chunk, n);
    }

    const std::size_t header_end = response_text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "控制面响应不完整（缺少头块结束）";
        return false;
    }
    const std::string headers = response_text.substr(0, header_end);
    const auto first_line_end = headers.find("\r\n");
    const std::string status_line = headers.substr(0, first_line_end);
    const auto status_sep = status_line.find(' ');
    const int status = status_sep == std::string::npos
                           ? 0
                           : std::atoi(status_line.substr(status_sep + 1).c_str());
    const std::string response_body = response_text.substr(header_end + 4);
    try {
        response = json::parse(response_body);
    } catch (const std::exception& parse_error) {
        error = std::string{"控制面响应无法解析（HTTP "} + std::to_string(status) +
                "）: " + parse_error.what();
        return false;
    }
    return true;
}

} // namespace

bool InstanceRunning() {
    int port = 0;
    std::string token;
    std::string ignored;
    if (!ReadEndpoint(port, token, ignored)) return false;
    // 只"能连上"不够：端点文件在监听成功后立刻写出，而命令投递口要等首次组合才挂上。
    // 用 ping 走一遍完整链路（含应用线程投递），这才是"实例可服务"。
    json response;
    if (!RoundTrip(port, token, {"ping"}, response, ignored, kProbeTimeoutMs)) return false;
    return response.contains("exit") && response.value("exit", 1) == 0;
}

bool StartInstanceAndWait(std::string& error) {
    if (InstanceRunning()) return true;
    if (!SpawnInstance(error)) return false;
    const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kStartupPollInterval);
        if (InstanceRunning()) return true;
    }
    error = "已拉起 apitab，但控制面端点在 30s 内未就绪（实例可能启动失败）。";
    return false;
}

namespace {

// 端点文件放运行目录（POSIX）以避免写用户数据目录；Windows 没有对应概念，回落
// 数据目录。与写端（control.cpp 的 EndpointFile）必须一致。
std::string RuntimeDirectory() {
#ifdef _WIN32
    return {};
#else
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return dir != nullptr && *dir != '\0' ? std::string{dir} : std::string{};
#endif
}

std::filesystem::path EndpointPath() {
    const std::string runtime = RuntimeDirectory();
    if (!runtime.empty()) return std::filesystem::path(runtime) / "apitab-control.json";
    return cfg::dataDir() / "control.json";
}

} // namespace

std::string EndpointFilePath() { return EndpointPath().string(); }

bool ReadEndpoint(int& port, std::string& token, std::string& error) {
    const std::filesystem::path path = EndpointPath();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "apitab 未在运行（找不到控制面端点 " + path.string() +
                "）。先启动 apitab，或用 apitab --ensure 拉起。";
        return false;
    }
    try {
        json payload;
        input >> payload;
        port = payload.at("port").get<int>();
        token = payload.at("token").get<std::string>();
    } catch (const std::exception& parse_error) {
        error = "控制面端点文件不可用（" + path.string() + "）: " + parse_error.what();
        return false;
    }
    return true;
}

bool IsHelpOnly(const std::vector<std::string>& args) {
    if (args.empty()) return true;
    if (args.front() == "help" || args.front() == "--help" || args.front() == "-h") return true;
    for (const std::string& item : args) {
        if (item == "--help" || item == "-h") return true;
    }
    return false;
}

bool ExchangeCommand(const std::vector<std::string>& args, CommandResult& result,
                     std::string& error) {
    int port = 0;
    std::string token;
    if (!ReadEndpoint(port, token, error)) return false;

    json response;
    if (!RoundTrip(port, token, args, response, error, kForwardTimeoutMs)) return false;
    if (!response.contains("exit")) {
        error = response.value("error", std::string{"控制面返回错误"});
        return false;
    }
    result.exit_code = response.value("exit", 1);
    result.stdout_text = response.value("stdout", std::string{});
    result.stderr_text = response.value("stderr", std::string{});
    return true;
}

int ForwardCommand(const std::vector<std::string>& args, std::string& error, bool ensure) {
    if (ensure && !StartInstanceAndWait(error)) return 1;

    CommandResult result;
    if (!ExchangeCommand(args, result, error)) return 1;

    // stdout 只放数据、stderr 放提示——与本地直跑时的分流完全一致。
    std::cout << result.stdout_text;
    std::cout.flush();
    std::cerr << result.stderr_text;
    std::cerr.flush();
    return result.exit_code;
}

} // namespace apitab::control
