// test_control_client.cpp — 控制面**客户端半边**的协议契约回归测试。
//
// 控制面服务端在 GUI 进程里（要跑 HuxerUI runtime 才能起），本测试不碰它，而是用
// 一条本地回环 HTTP 桩冒充实例，验证客户端侧真正容易写错、且 agent 直接依赖的三件事：
//
//   1) **结果回放契约**：`{"exit","stdout","stderr"}` 必须原样变成客户端的退出码与
//      两路输出（stdout 只放数据、stderr 放提示——与命令在实例内直跑时一致）；
//   2) **错误路径**：端点文件缺失 / token 不匹配 / 非 200 响应，必须给出可读错误并让
//      调用方拿到 false，而不是静默成功或崩溃；
//   3) **就绪探测**：`InstanceRunning()` 用 `ping` 走完整链路判断"实例可服务"——
//      端点文件存在但服务端不认 ping（比如投递口还没挂上）时必须判为未就绪。
//
// 平台：POSIX（Linux/macOS）执行完整断言；Windows 只保留编译（回环桩基于 BSD socket，
// 与 test_curl_engine 同一取舍），打印跳过并返回 0。
#ifndef _WIN32

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "control.h"

import asio;
import nlohmann.json;

namespace {

using json = nlohmann::json;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

// 极简 HTTP 桩：固定响应体，或按请求内容回一个可编程结果。
class StubServer {
public:
    // respond: 收到一条合法请求后返回 (status, body)。
    using Responder = std::function<std::pair<int, std::string>(const std::string& body)>;

    bool Start(Responder responder) {
        responder_ = std::move(responder);
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // 临时端口
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listen_fd_, 8) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    void Stop() {
        stopping_.store(true);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    ~StubServer() { Stop(); }

    int port() const { return port_; }

private:
    void Serve() {
        for (;;) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) return;
            Handle(fd);
            ::close(fd);
        }
    }

    void Handle(int fd) {
        std::string request;
        char chunk[2048];
        for (;;) {
            const std::size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                // 简化：本测试的请求体都很小，一次读到的就够；不足再补读。
                const std::size_t body_start = header_end + 4;
                std::size_t content_length = 0;
                const auto cl = request.find("Content-Length: ");
                if (cl != std::string::npos) content_length = std::stoul(request.substr(cl + 16));
                while (request.size() - body_start < content_length) {
                    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                    if (n <= 0) return;
                    request.append(chunk, static_cast<std::size_t>(n));
                }
                break;
            }
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            request.append(chunk, static_cast<std::size_t>(n));
        }

        const auto header_end = request.find("\r\n\r\n");
        const auto auth = request.find("Authorization: Bearer ");
        const std::string token =
            auth == std::string::npos ? std::string{}
                                      : request.substr(auth + 22, request.find("\r\n", auth) - (auth + 22));
        const std::string body = request.substr(header_end + 4);

        if (!token_.empty() && token != token_) {
            Send(fd, 401, json{{"error", "unauthorized"}}.dump());
            return;
        }
        auto [status, payload] = responder_(body);
        Send(fd, status, payload);
    }

    static void Send(int fd, int status, const std::string& body) {
        std::string response = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;
        ::send(fd, response.data(), response.size(), 0);
    }

public:
    std::string token_{"test-token"};

private:
    Responder responder_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
};

// 把端点文件写到 XDG_RUNTIME_DIR 下（客户端就是按这个路径找实例的）。
std::string WriteEndpoint(int port, const std::string& token) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                      ("apitab-control-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "apitab-control.json";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << json{{"port", port}, {"token", token}, {"pid", 0}}.dump();
    out.close();
    ::setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    return path.string();
}

} // namespace

int main() {
    StubServer server;
    // 默认：把 {"args":[...]} 原样当成命令名回一个成功结果。
    const bool started = server.Start([](const std::string& body) {
        json request = json::parse(body);
        std::string command;
        for (const auto& item : request.at("args")) {
            if (!command.empty()) command.push_back(' ');
            command += item.get<std::string>();
        }
        if (command == "ping") {
            return std::make_pair(200, json{{"exit", 0}, {"stdout", "pong\n"}, {"stderr", ""}}.dump());
        }
        if (command.starts_with("fail")) {
            return std::make_pair(200, json{{"exit", 1}, {"stdout", ""}, {"stderr", "boom\n"}}.dump());
        }
        if (command.starts_with("json")) {
            return std::make_pair(200,
                                  json{{"exit", 0}, {"stdout", "{\"a\":1}\n"}, {"stderr", ""}}.dump());
        }
        return std::make_pair(200,
                              json{{"exit", 0}, {"stdout", "data " + command + "\n"}, {"stderr", ""}}.dump());
    });
    if (!started) {
        std::printf("桩服务启动失败，跳过\n");
        return 0;
    }

    const std::string endpoint = WriteEndpoint(server.port(), server.token_);
    std::printf("端点: %s\n", endpoint.c_str());

    // 1) 结果回放：stdout / 退出码
    {
        apitab::control::CommandResult result;
        std::string error;
        const bool ok = apitab::control::ExchangeCommand({"orgs"}, result, error);
        Check(ok, "ExchangeCommand(orgs) 成功");
        Check(result.exit_code == 0, "退出码 0 原样回传");
        Check(result.stdout_text == "data orgs\n", "stdout 原样回传");
        Check(result.stderr_text.empty(), "stderr 为空");
    }

    // 2) 失败命令：非 0 退出码 + stderr 提示
    {
        apitab::control::CommandResult result;
        std::string error;
        const bool ok = apitab::control::ExchangeCommand({"fail"}, result, error);
        Check(ok, "命令失败仍算一次成功往返（结果是退出码 1）");
        Check(result.exit_code == 1, "非 0 退出码回传");
        Check(result.stderr_text == "boom\n", "stderr 原样回传");
    }

    // 3) --json 结果不被改写（客户端不做任何格式化）
    {
        apitab::control::CommandResult result;
        std::string error;
        (void)apitab::control::ExchangeCommand({"json"}, result, error);
        Check(result.stdout_text == "{\"a\":1}\n", "--json 输出原样透传");
    }

    // 4) 就绪探测：ping 通过 → InstanceRunning() 为真
    Check(apitab::control::InstanceRunning(), "ping 成功时 InstanceRunning() == true");

    // 5) token 不匹配 → 服务端 401 → 客户端给出错误而不是假成功
    {
        WriteEndpoint(server.port(), "wrong-token");
        apitab::control::CommandResult result;
        std::string error;
        const bool ok = apitab::control::ExchangeCommand({"orgs"}, result, error);
        Check(!ok, "token 不匹配时 ExchangeCommand 失败");
        Check(error.find("unauthorized") != std::string::npos, "错误里带 unauthorized");
        WriteEndpoint(server.port(), server.token_);
    }

    // 6) 端点文件缺失 → 可读错误（提示先启动实例）
    {
        ::setenv("XDG_RUNTIME_DIR", "/tmp/apitab-control-nonexistent", 1);
        apitab::control::CommandResult result;
        std::string error;
        const bool ok = apitab::control::ExchangeCommand({"orgs"}, result, error);
        Check(!ok, "没有端点文件时失败");
        Check(error.find("未在运行") != std::string::npos, "错误提示实例未运行");
        Check(!apitab::control::InstanceRunning(), "没有端点时 InstanceRunning() == false");
        WriteEndpoint(server.port(), server.token_);
    }

    // 7) help 判定：纯文本命令不需要实例（平台入口据此本地打印）
    Check(apitab::control::IsHelpOnly({}), "无参数 = help");
    Check(apitab::control::IsHelpOnly({"help"}), "help = help");
    Check(apitab::control::IsHelpOnly({"requests", "--help"}), "子命令 --help = help");
    Check(!apitab::control::IsHelpOnly({"orgs"}), "orgs 不是 help");

    // 8) 服务端停掉后：InstanceRunning() 必须转为 false（不能只看端点文件在不在）
    server.Stop();
    Check(!apitab::control::InstanceRunning(), "服务端停掉后 InstanceRunning() == false");

    std::filesystem::remove_all(std::filesystem::path(endpoint).parent_path());
    std::printf(g_failures == 0 ? "control_client: 全部通过\n" : "control_client: %d 项失败\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}

#else

#include <cstdio>

int main() {
    std::printf("control_client 测试在 Windows 上跳过（回环桩基于 BSD socket）\n");
    return 0;
}

#endif
