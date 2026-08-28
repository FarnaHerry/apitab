// tcp_engine.cpp — Asio 原始 TCP / TCPS 实现；回调只投递事件并唤醒 UI。
// asio 全部经 `import asio;`（cmake/asio.cppm）；不要在全局片段里 #include asio 头
// —— 模块 BMI 与头文件各自的内部链接静态对象会在 GCC 下重复定义。
module;

#include <openssl/ssl.h>

namespace core::platform { void requestUiUpdate(); }

module apitab.tcp_engine;

import std;
import asio;
import apitab.api_engine;

namespace {

using Tcp = asio::ip::tcp;

struct Endpoint {
    bool tls = false;
    std::string host;
    std::string port;
};

std::string parseEndpoint(const std::string& raw, Endpoint& out) {
    const std::string url = raw;
    const bool tcp = url.starts_with("tcp://");
    const bool tcps = url.starts_with("tcps://");
    if (!tcp && !tcps) return "TCP 地址必须以 tcp:// 或 tcps:// 开头";
    if (url.find_first_of("/?#", tcp ? 6 : 7) != std::string::npos ||
        url.find('@') != std::string::npos || url.find_first_of(" \t\r\n") != std::string::npos) {
        return "TCP 地址只能包含主机和端口";
    }
    const std::string_view authority(url.data() + (tcp ? 6 : 7),
                                     url.size() - (tcp ? 6 : 7));
    if (authority.empty()) return "TCP 地址缺少主机和端口";

    std::string_view host;
    std::string_view port;
    if (authority.front() == '[') {
        const std::size_t end = authority.find(']');
        if (end == std::string_view::npos || end + 1 >= authority.size() || authority[end + 1] != ':') {
            return "IPv6 地址必须使用 [地址]:端口";
        }
        host = authority.substr(1, end - 1);
        port = authority.substr(end + 2);
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos || authority.find(':') != colon) {
            return "TCP 地址必须包含明确端口";
        }
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    }
    if (host.empty() || port.empty() || !std::ranges::all_of(port, [](unsigned char c) { return c >= '0' && c <= '9'; })) {
        return "TCP 地址中的主机或端口无效";
    }
    unsigned int number = 0;
    for (const char c : port) {
        number = number * 10U + static_cast<unsigned int>(c - '0');
        if (number > 65535U) return "TCP 端口必须在 1 到 65535 之间";
    }
    if (number == 0) return "TCP 端口必须在 1 到 65535 之间";
    out = Endpoint{.tls = tcps, .host = std::string(host), .port = std::string(port)};
    return {};
}

class AsioTcpEngine final : public api::TcpEngine {
public:
    AsioTcpEngine()
        : work_(asio::make_work_guard(io_)), thread_([this] { io_.run(); }) {}

    ~AsioTcpEngine() override {
        disconnect();
        work_.reset();
        io_.stop();
        if (thread_.joinable()) thread_.join();
    }

    std::string connect(const api::TcpSpec& spec) override {
        Endpoint endpoint;
        if (const std::string error = parseEndpoint(spec.url, endpoint); !error.empty()) return error;
        disconnect();

        const std::uint64_t generation = ++generation_;
        auto session = std::make_shared<Session>(io_, endpoint.tls);
        session->endpoint = std::move(endpoint);
        session->timeout = std::max(1, spec.connectTimeoutSec);
        {
            std::lock_guard lock(sessionMutex_);
            session_ = session;
        }
        setState(api::TcpState::Resolving);
        push({.kind = api::TcpEventKind::Connecting, .detail = "正在解析 " + session->endpoint.host + ":" + session->endpoint.port});
        asio::post(io_, [this, session, generation] { beginResolve(session, generation); });
        return {};
    }

    void disconnect() override {
        const std::uint64_t generation = ++generation_;
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(sessionMutex_);
            session = std::exchange(session_, {});
        }
        if (!session) {
            setState(api::TcpState::Disconnected);
            return;
        }
        asio::post(io_, [this, session, generation] {
            asio::error_code ignored;
            session->timer.cancel();
            session->resolver.cancel();
            std::visit([&](auto& stream) {
                stream.lowest_layer().cancel(ignored);
                stream.lowest_layer().shutdown(Tcp::socket::shutdown_both, ignored);
                stream.lowest_layer().close(ignored);
            }, session->stream);
        });
        setState(api::TcpState::Disconnected);
        push({.kind = api::TcpEventKind::Disconnected, .detail = "已断开"});
    }

    std::string send(const std::vector<std::uint8_t>& bytes) override {
        if (bytes.empty()) return "消息不能为空";
        if (state() != api::TcpState::Connected) return "TCP 尚未连接";
        std::shared_ptr<Session> session;
        {
            std::lock_guard lock(sessionMutex_);
            session = session_;
        }
        if (!session) return "TCP 尚未连接";
        const std::uint64_t generation = generation_.load();
        asio::post(io_, [this, session, generation, bytes] {
            if (!isCurrent(session, generation)) return;
            session->writes.push_back(bytes);
            if (!session->writing) writeNext(session, generation);
        });
        return {};
    }

    api::TcpState state() const override {
        std::lock_guard lock(stateMutex_);
        return state_;
    }

    std::vector<api::TcpEvent> drainEvents() override {
        std::lock_guard lock(eventsMutex_);
        return std::exchange(events_, {});
    }

private:
    using Stream = std::variant<Tcp::socket, asio::ssl::stream<Tcp::socket>>;

    struct Session {
        explicit Session(asio::io_context& io, bool tls)
            : resolver(io), timer(io), sslContext(asio::ssl::context::tls_client),
              stream(tls ? Stream{std::in_place_index<1>, io, sslContext}
                         : Stream{std::in_place_index<0>, io}) {}
        Endpoint endpoint;
        Tcp::resolver resolver;
        asio::steady_timer timer;
        asio::ssl::context sslContext;
        Stream stream;
        std::array<std::uint8_t, 16384> readBuffer{};
        std::deque<std::vector<std::uint8_t>> writes;
        bool writing = false;
        int timeout = 15;
    };

    bool isCurrent(const std::shared_ptr<Session>& session, std::uint64_t generation) const {
        if (generation != generation_.load()) return false;
        std::lock_guard lock(sessionMutex_);
        return session_ == session;
    }

    void beginResolve(const std::shared_ptr<Session>& session, std::uint64_t generation) {
        if (!isCurrent(session, generation)) return;
        session->timer.expires_after(std::chrono::seconds(session->timeout));
        session->timer.async_wait([this, session, generation](const asio::error_code& ec) {
            if (ec || !isCurrent(session, generation)) return;
            fail(session, generation, "TCP 连接超时");
        });
        session->resolver.async_resolve(session->endpoint.host, session->endpoint.port,
            [this, session, generation](const asio::error_code& ec, const auto& results) {
                if (!isCurrent(session, generation)) return;
                if (ec) return fail(session, generation, "DNS 解析失败: " + ec.message());
                setState(api::TcpState::Connecting);
                std::visit([&](auto& stream) {
                    asio::async_connect(stream.lowest_layer(), results,
                        [this, session, generation](const asio::error_code& connectEc, const auto&) {
                            if (!isCurrent(session, generation)) return;
                            if (connectEc) return fail(session, generation, "TCP 连接失败: " + connectEc.message());
                            connectedTcp(session, generation);
                        });
                }, session->stream);
            });
    }

    void connectedTcp(const std::shared_ptr<Session>& session, std::uint64_t generation) {
        if (!session->endpoint.tls) return connected(session, generation);
        setState(api::TcpState::Handshaking);
        auto& stream = std::get<asio::ssl::stream<Tcp::socket>>(session->stream);
        session->sslContext.set_default_verify_paths();
        stream.set_verify_mode(asio::ssl::context::verify_peer);
        stream.set_verify_callback(asio::ssl::host_name_verification(session->endpoint.host));
        if (SSL_set_tlsext_host_name(stream.native_handle(), session->endpoint.host.c_str()) != 1) {
            return fail(session, generation, "无法设置 TLS SNI 主机名");
        }
        stream.async_handshake(asio::ssl::stream_base::client,
            [this, session, generation](const asio::error_code& ec) {
                if (!isCurrent(session, generation)) return;
                if (ec) return fail(session, generation, "TLS 握手失败: " + ec.message());
                connected(session, generation);
            });
    }

    void connected(const std::shared_ptr<Session>& session, std::uint64_t generation) {
        asio::error_code ignored;
                session->timer.cancel();
        setState(api::TcpState::Connected);
        push({.kind = api::TcpEventKind::Connected,
              .detail = std::string(session->endpoint.tls ? "TCPS 已连接: " : "TCP 已连接: ") +
                        session->endpoint.host + ":" + session->endpoint.port});
        readNext(session, generation);
    }

    void readNext(const std::shared_ptr<Session>& session, std::uint64_t generation) {
        std::visit([&](auto& stream) {
            stream.async_read_some(asio::buffer(session->readBuffer),
                [this, session, generation](const asio::error_code& ec, std::size_t count) {
                    if (!isCurrent(session, generation)) return;
                    if (ec) {
                        if (ec == asio::error::eof) {
                            setState(api::TcpState::Disconnected);
                            push({.kind = api::TcpEventKind::Disconnected, .detail = "远端关闭连接"});
                        } else {
                            fail(session, generation, "TCP 接收失败: " + ec.message());
                        }
                        return;
                    }
                    api::TcpEvent event;
                    event.kind = api::TcpEventKind::Received;
                    event.wireBytes = count;
                    event.payload.assign(session->readBuffer.begin(), session->readBuffer.begin() + static_cast<std::ptrdiff_t>(count));
                    push(std::move(event));
                    readNext(session, generation);
                });
        }, session->stream);
    }

    void writeNext(const std::shared_ptr<Session>& session, std::uint64_t generation) {
        if (session->writes.empty()) { session->writing = false; return; }
        session->writing = true;
        const auto& bytes = session->writes.front();
        std::visit([&](auto& stream) {
            asio::async_write(stream, asio::buffer(bytes),
                [this, session, generation](const asio::error_code& ec, std::size_t count) {
                    if (!isCurrent(session, generation)) return;
                    if (ec) return fail(session, generation, "TCP 发送失败: " + ec.message());
                    api::TcpEvent event;
                    event.kind = api::TcpEventKind::Sent;
                    event.wireBytes = count;
                    event.payload = std::move(session->writes.front());
                    session->writes.pop_front();
                    push(std::move(event));
                    writeNext(session, generation);
                });
        }, session->stream);
    }

    void fail(const std::shared_ptr<Session>& session, std::uint64_t generation, std::string detail) {
        if (!isCurrent(session, generation)) return;
        asio::error_code ignored;
                session->timer.cancel();
        std::visit([&](auto& stream) { stream.lowest_layer().close(ignored); }, session->stream);
        setState(api::TcpState::Failed);
        push({.kind = api::TcpEventKind::Error, .detail = std::move(detail)});
    }

    void setState(api::TcpState state) {
        std::lock_guard lock(stateMutex_);
        state_ = state;
    }

    void push(api::TcpEvent event) {
        {
            std::lock_guard lock(eventsMutex_);
            if (events_.size() >= kMaxEvents) events_.erase(events_.begin());
            events_.push_back(std::move(event));
        }
        core::platform::requestUiUpdate();
    }

    static constexpr std::size_t kMaxEvents = 2000;
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
    std::atomic<std::uint64_t> generation_{0};
    mutable std::mutex sessionMutex_;
    std::shared_ptr<Session> session_;
    mutable std::mutex stateMutex_;
    api::TcpState state_ = api::TcpState::Disconnected;
    std::mutex eventsMutex_;
    std::vector<api::TcpEvent> events_;
};

} // namespace

std::unique_ptr<api::TcpEngine> makeTcpEngine() {
    return std::make_unique<AsioTcpEngine>();
}
