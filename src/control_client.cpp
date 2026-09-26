// control_client.cpp — `apitab --cli …` 的客户端半边（见 control.h）。
//
// 这个进程**不构造 store、不碰数据库、不进事件循环**：它只读端点文件、把 argv 发给
// 运行中的实例、把回传的 stdout/stderr 原样回放并沿用它的退出码。命令实现在实例里
// （src/cli.cpp 同一份），所以输出契约与 "--cli 在实例内直跑" 完全一致。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#include "control.h"

import asio;
import nlohmann.json;

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
    const BOOL ok = ::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
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
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
        error = "拉起 apitab 失败: " + std::string{std::strerror(rc)};
        return false;
    }
    return true;
#endif
}

} // namespace

bool InstanceRunning() {
    int port = 0;
    std::string token;
    std::string ignored;
    if (!ReadEndpoint(port, token, ignored)) return false;
    asio::io_context context;
    asio::ip::tcp::socket socket{context};
    asio::error_code ec;
    socket.connect({asio::ip::make_address("127.0.0.1", ec),
                    static_cast<unsigned short>(port)}, ec);
    return !ec;
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

int ForwardCommand(const std::vector<std::string>& args, std::string& error, bool ensure) {
    if (ensure && !StartInstanceAndWait(error)) return 1;
    int port = 0;
    std::string token;
    if (!ReadEndpoint(port, token, error)) return 1;

    asio::io_context context;
    asio::ip::tcp::socket socket{context};
    asio::error_code ec;
    const asio::ip::tcp::endpoint endpoint{
        asio::ip::make_address("127.0.0.1", ec), static_cast<unsigned short>(port)};
    if (ec) {
        error = std::string{"控制面端点无效: "} + ec.message();
        return 1;
    }
    socket.connect(endpoint, ec);
    if (ec) {
        error = std::string{"连接运行中的 apitab 失败（127.0.0.1:"} + std::to_string(port) +
                "）: " + ec.message();
        return 1;
    }

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
        return 1;
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
            return 1;
        }
        response_text.append(chunk, n);
    }

    const std::size_t header_end = response_text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "控制面响应不完整（缺少头块结束）";
        return 1;
    }
    const std::string headers = response_text.substr(0, header_end);
    const auto first_line_end = headers.find("\r\n");
    const std::string status_line = headers.substr(0, first_line_end);
    const auto status_sep = status_line.find(' ');
    const int status = status_sep == std::string::npos
                           ? 0
                           : std::atoi(status_line.substr(status_sep + 1).c_str());
    const std::string response_body = response_text.substr(header_end + 4);

    json response;
    try {
        response = json::parse(response_body);
    } catch (const std::exception& parse_error) {
        error = std::string{"控制面响应无法解析（HTTP "} + std::to_string(status) +
                "）: " + parse_error.what();
        return 1;
    }

    // stdout 只放数据、stderr 放提示——与本地直跑时的分流完全一致。
    std::cout << response.value("stdout", std::string{});
    std::cout.flush();
    std::cerr << response.value("stderr", std::string{});
    std::cerr.flush();
    return response.value("exit", 1);
}

} // namespace apitab::control
