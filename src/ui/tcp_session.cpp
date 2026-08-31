// tcp_session.cpp — TcpSession 实现：全同步 asio，不拥有线程。
// 连接/超时用 async 链 + 调用线程自跑 io_context.run()（run 返回即结束，不产生
// 常驻线程）；读写用同步调用。地址解析与 TLS 逻辑对照原 tcp_engine.cpp（已删除）。
// asio 走 `import asio;`（cmake/asio.cppm）；不要无条件 #include asio 头 —— 模块
// BMI 与头文件各自的内部链接静态对象会在 GCC 下重复定义（CLAUDE.md 约定 4）。
// MSVC 例外：import asio 触发 IFC 编译器错误（C1116），退回纯头文件 ——
// ASIO_SEPARATE_COMPILATION 下实现仍在 asio 库里，无重复。
// 注意 include 顺序：文本包含必须全部在 tcp_session.h（其内 import 了带
// `import std` 的模块）之前，否则 BMI 与文本头实体重复定义（约定 1 的变体）。
#include <openssl/ssl.h>
#ifdef _MSC_VER
#include <asio.hpp>
#include <asio/ssl.hpp>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <optional>
#include <utility>
#include <variant>

#include "tcp_session.h"

#ifndef _MSC_VER
import asio;
#endif

namespace apitab::ui {

namespace {

using Tcp = asio::ip::tcp;
using Stream = std::variant<Tcp::socket, asio::ssl::stream<Tcp::socket>>;

struct Endpoint {
    bool tls = false;
    std::string host;
    std::string port;
};

std::string parseEndpoint(const std::string& url, Endpoint& out) {
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
    if (host.empty() || port.empty() ||
        !std::ranges::all_of(port, [](unsigned char c) { return c >= '0' && c <= '9'; })) {
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

void closeLowest(Stream& stream) {
    std::visit([](auto& s) {
        asio::error_code ec;
        s.lowest_layer().shutdown(Tcp::socket::shutdown_both, ec);
        s.lowest_layer().close(ec);
    }, stream);
}

} // namespace

struct TcpSession::Impl {
    asio::io_context io;
    Tcp::resolver resolver{io};
    asio::steady_timer timer{io};
    asio::ssl::context sslContext{asio::ssl::context::tls_client};
    std::optional<Stream> stream;
    std::atomic<bool> connected{false};
    std::atomic<bool> closed{false};
};

TcpSession::TcpSession() : impl_(std::make_unique<Impl>()) {}

TcpSession::~TcpSession() {
    close();
}

std::string TcpSession::connect(const api::TcpSpec& spec) {
    Endpoint endpoint;
    if (const std::string error = parseEndpoint(spec.url, endpoint); !error.empty()) return error;
    Impl& impl = *impl_;
    if (impl.closed.load()) return "连接已取消";
    impl.stream.emplace(endpoint.tls
                            ? Stream{std::in_place_index<1>, impl.io, impl.sslContext}
                            : Stream{std::in_place_index<0>, impl.io});

    // 异步链 + 截止定时器，io.run() 直接在调用线程（任务线程）上跑：完成/超时/
    // 被 close() 打断即返回，不产生常驻线程。
    std::string result;
    bool ok = false;
    impl.timer.expires_after(std::chrono::seconds(std::max(1, spec.connectTimeoutSec)));
    impl.timer.async_wait([&](const asio::error_code& ec) {
        if (ec) return; // 被 cancel：正常完成路径
        result = "TCP 连接超时";
        if (impl.stream) closeLowest(*impl.stream);
    });
    impl.resolver.async_resolve(endpoint.host, endpoint.port,
        [&](const asio::error_code& ec, const auto& results) {
            if (ec) {
                result = "DNS 解析失败: " + ec.message();
                return;
            }
            std::visit([&](auto& stream) {
                asio::async_connect(stream.lowest_layer(), results,
                    [&](const asio::error_code& connectEc, const auto&) {
                        if (connectEc) {
                            result = "TCP 连接失败: " + connectEc.message();
                            return;
                        }
                        if (!endpoint.tls) {
                            ok = true;
                            return;
                        }
                        auto& tls = std::get<asio::ssl::stream<Tcp::socket>>(*impl.stream);
                        impl.sslContext.set_default_verify_paths();
                        tls.set_verify_mode(asio::ssl::context::verify_peer);
                        tls.set_verify_callback(asio::ssl::host_name_verification(endpoint.host));
                        if (SSL_set_tlsext_host_name(tls.native_handle(), endpoint.host.c_str()) != 1) {
                            result = "无法设置 TLS SNI 主机名";
                            return;
                        }
                        tls.async_handshake(asio::ssl::stream_base::client,
                            [&](const asio::error_code& handshakeEc) {
                                if (handshakeEc) {
                                    result = "TLS 握手失败: " + handshakeEc.message();
                                    return;
                                }
                                ok = true;
                            });
                    });
            }, *impl.stream);
        });
    impl.io.run();
    impl.io.restart();
    impl.timer.cancel();

    if (ok) {
        impl.connected = true;
        return {};
    }
    if (impl.stream) closeLowest(*impl.stream);
    if (result.empty()) result = "连接已取消"; // close() 打断
    return result;
}

std::string TcpSession::send(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return "消息不能为空";
    Impl& impl = *impl_;
    if (!impl.connected.load() || !impl.stream) return "TCP 尚未连接";
    // 同步写（writeTimeoutSec 不再单独实现：本机调试场景写缓冲极少打满）。
    asio::error_code ec;
    std::visit([&](auto& stream) { asio::write(stream, asio::buffer(bytes), ec); },
               *impl.stream);
    if (ec) {
        impl.connected = false;
        return "TCP 发送失败: " + ec.message();
    }
    return {};
}

api::TcpEvent TcpSession::read() {
    Impl& impl = *impl_;
    api::TcpEvent event;
    if (!impl.connected.load() || !impl.stream) {
        event.kind = api::TcpEventKind::Disconnected;
        event.detail = "已断开";
        return event;
    }
    // 同步阻塞读：close() 的 shutdown 会把它唤醒（eof/错误）。
    std::array<std::uint8_t, 16384> buffer;
    asio::error_code ec;
    const std::size_t count = std::visit(
        [&](auto& stream) { return stream.read_some(asio::buffer(buffer), ec); },
        *impl.stream);
    if (!ec) {
        event.kind = api::TcpEventKind::Received;
        event.wireBytes = count;
        event.payload.assign(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        return event;
    }
    impl.connected = false;
    if (ec == asio::error::eof) {
        event.kind = api::TcpEventKind::Disconnected;
        event.detail = "远端关闭连接";
    } else if (impl.closed.load()) {
        event.kind = api::TcpEventKind::Disconnected;
        event.detail = "已断开";
    } else {
        event.kind = api::TcpEventKind::Error;
        event.detail = "TCP 接收失败: " + ec.message();
    }
    return event;
}

bool TcpSession::isConnected() const {
    return impl_->connected.load();
}

void TcpSession::close() {
    Impl& impl = *impl_;
    if (impl.closed.exchange(true)) return;
    impl.connected = false;
    // 跨线程打断阻塞中的操作：cancel 定时器/解析器（connect 阶段），
    // shutdown+close 套接字（read 阶段，POSIX 下唤醒阻塞的 recv）。
    impl.timer.cancel();
    impl.resolver.cancel();
    if (impl.stream) closeLowest(*impl.stream);
}

} // namespace apitab::ui
