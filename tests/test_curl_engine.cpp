// test_curl_engine.cpp — 单次请求引擎（apitab.curl_engine）的资源契约回归测试。
//
// 用一条本地回环 HTTP/1.1 服务器统计"服务端看到的连接数 / 请求数"，验证三件事：
//   1) 连接复用：引擎常驻 easy 句柄，连续两次请求必须复用同一条 TCP 连接
//      （connections == 1）。若退回"每请求 curl_easy_init/cleanup"，第二条连接
//      立即出现，本测试失败；
//   2) 代理接线：RequestSpec::proxy 必须真的作用到本次传输（连不上代理即失败），
//      而不是被静默忽略；
//   3) 选项复位：上一请求设置的代理不得漏到下一请求（请求 4 必须成功直连）。
//
// 平台：POSIX（Linux/macOS）执行完整断言；Windows 只保留编译（本测试用的
// 回环服务器基于 BSD socket，未做 winsock 移植），打印跳过并返回 0。

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

import std;
import apitab.api_engine;
import apitab.curl_engine;

// 引擎完成传输后调用本钩子唤醒 UI（GUI 里由 app.cpp 实现，测试里是空实现）。
namespace core::platform {
void requestUiUpdate() {}
} // namespace core::platform

namespace {

#ifndef _WIN32

// 极简回环 HTTP/1.1 服务器：固定 200 + "ok" 响应，keep-alive 循环读同一条连接。
class LoopbackHttpServer {
public:
    bool start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) return false;
        int one = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // 临时端口，避免与并行测试抢占
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listenFd_, 8) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port_ = ntohs(addr.sin_port);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    // 停服 = 关监听套接字 + 置停止位；连接处理循环用 poll 超时兜底退出。
    void stop() {
        stopping_.store(true);
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    ~LoopbackHttpServer() { stop(); }

    std::string url() const { return std::format("http://127.0.0.1:{}/", port_); }
    int connections() const { return connections_.load(); }
    int requests() const { return requests_.load(); }

private:
    void serve() {
        for (;;) {
            const int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0) return; // 监听套接字被 stop() 关闭
            connections_.fetch_add(1);
            handleConnection(fd);
        }
    }

    void handleConnection(int fd) {
        std::string pending;
        for (;;) {
            pollfd pfd{fd, POLLIN, 0};
            const int ready = ::poll(&pfd, 1, 100);
            if (ready < 0) break;
            if (ready == 0) {
                if (stopping_.load()) break; // 客户端仍持连接：靠超时收摊
                continue;
            }
            char buffer[2048];
            const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            pending.append(buffer, static_cast<std::size_t>(n));
            // 本测试的请求都没有正文，头块结束即一个完整请求。
            for (;;) {
                const std::size_t end = pending.find("\r\n\r\n");
                if (end == std::string::npos) break;
                pending.erase(0, end + 4);
                requests_.fetch_add(1);
                static constexpr std::string_view kResponse =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";
                if (::send(fd, kResponse.data(), kResponse.size(), 0) < 0) {
                    ::close(fd);
                    return;
                }
            }
        }
        ::close(fd);
    }

    int listenFd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> connections_{0};
    std::atomic<int> requests_{0};
    std::thread thread_;
};

std::optional<api::ResponseView> waitForResponse(api::ApiEngine& engine,
                                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        api::ResponseView out;
        if (engine.takeResponse(out)) return out;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

int fail(std::string_view what) {
    std::println(std::cerr, "test_curl_engine: {}", what);
    return 1;
}

// 直连 GET 规格（避免逐字段指定初始化在 -Wextra 下的缺字段告警）。
api::RequestSpec plainGet(const std::string& url) {
    api::RequestSpec spec;
    spec.method = "GET";
    spec.url = url;
    return spec;
}

#endif // !_WIN32

} // namespace

int main() {
#ifdef _WIN32
    std::println("test_curl_engine: skipped on Windows (loopback server is POSIX-only)");
    return 0;
#else
    LoopbackHttpServer server;
    if (!server.start()) return fail("回环服务器启动失败");

    std::unique_ptr<api::ApiEngine> engine = makeCurlEngine();
    const std::string url = server.url();
    constexpr auto kTimeout = std::chrono::milliseconds{10000};

    const auto sendAndWait = [&](const api::RequestSpec& spec) {
        engine->send(spec);
        return waitForResponse(*engine, kTimeout);
    };

    // 1) 首个请求：建立唯一一条连接。
    const auto first = sendAndWait(plainGet(url));
    if (!first) return fail("首个请求超时（结果槽始终未填）");
    if (!first->ok || first->status != 200 || first->body != "ok")
        return fail(std::format("首个请求失败: ok={} status={} error={} body='{}'",
                                first->ok, first->status, first->error, first->body));
    if (server.connections() != 1 || server.requests() != 1)
        return fail(std::format("首个请求后的服务端计数异常: connections={} requests={}",
                                server.connections(), server.requests()));

    // 2) 第二个请求：必须复用常驻句柄里的连接（服务端不新增 accept）。
    const auto second = sendAndWait(plainGet(url));
    if (!second || !second->ok) return fail("第二个请求失败（常驻句柄复用路径）");
    if (server.connections() != 1)
        return fail(std::format("连接未被复用: 服务端看到 {} 条连接（期望 1）",
                                server.connections()));
    if (server.requests() != 2)
        return fail(std::format("请求计数异常: {}", server.requests()));

    // 3) 带不可达代理的请求：必须失败（证明 spec.proxy 真的接线到传输）。
    api::RequestSpec proxied = plainGet(url);
    proxied.proxy = "http://127.0.0.1:1";
    const auto third = sendAndWait(proxied);
    if (!third) return fail("代理请求超时");
    if (third->ok)
        return fail("设置了代理却直连成功：RequestSpec::proxy 未被应用到传输");
    if (server.connections() != 1)
        return fail(std::format("代理请求不应直达目标服务器（connections={}）",
                                server.connections()));

    // 4) 再发一次直连请求：必须成功 —— 上一请求的代理已被 curl_easy_reset 清掉。
    const auto fourth = sendAndWait(plainGet(url));
    if (!fourth || !fourth->ok)
        return fail("代理选项泄漏到了下一个请求（选项复位失败）");
    if (server.connections() != 1 || server.requests() != 3)
        return fail(std::format("直连请求 4 未复用连接: connections={} requests={}",
                                server.connections(), server.requests()));

    std::println("test_curl_engine: ok（1 条连接承载 3 次直连请求，代理按请求生效）");
    server.stop();
    engine.reset();
    return 0;
#endif
}
