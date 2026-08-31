// ws_session.cpp — WsSession 实现：每个会话一个 ix::WebSocket 实例。
// 事件逻辑对照原 websocket_engine.cpp（已删除）；会话对象即代际——重连 =
// 页面新建会话取代旧的，不再需要 generation/shuttingDown 守卫。
// 注意 include 顺序：文本包含必须全部在 ws_session.h（其内 import 了带
// `import std` 的模块）之前，否则 BMI 与文本头实体重复定义（约定 1 的变体）。
#include <algorithm>
#include <mutex>
#include <utility>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include "ws_session.h"

namespace apitab::ui {

namespace {
void InitNetSystemOnce() {
    // ix::initNetSystem 幂等性由 once 保证；进程生命周期内不 uninit（多会话
    // 并存/更替时 uninit 会误伤其它会话）。
    static std::once_flag flag;
    std::call_once(flag, [] { ix::initNetSystem(); });
}
} // namespace

struct WsSession::Impl {
    ix::WebSocket socket;
    std::mutex mu;
    api::WebSocketState state = api::WebSocketState::Disconnected;
    std::vector<api::WebSocketEvent> events;

    static constexpr std::size_t kMaxEvents = 2000;

    void setState(api::WebSocketState s) {
        std::lock_guard lock{mu};
        state = s;
    }

    // IX 线程回调：只投递事件，不碰 UI。
    void push(api::WebSocketEvent event) {
        std::lock_guard lock{mu};
        if (events.size() >= kMaxEvents) events.erase(events.begin());
        events.push_back(std::move(event));
    }
};

WsSession::WsSession() : impl_(std::make_unique<Impl>()) {
    InitNetSystemOnce();
}

WsSession::~WsSession() {
    // stop 会 join IX 线程；此后回调不会再触达本对象。
    impl_->socket.stop();
}

std::string WsSession::connect(const api::WebSocketSpec& spec) {
    if (!spec.url.starts_with("ws://") && !spec.url.starts_with("wss://")) {
        return "WebSocket 地址必须以 ws:// 或 wss:// 开头";
    }
    Impl& impl = *impl_;
    impl.socket.disableAutomaticReconnection();
    impl.socket.setUrl(spec.url);
    impl.socket.setHandshakeTimeout(std::max(1, spec.handshakeTimeoutSec));
    ix::WebSocketHttpHeaders headers;
    for (const auto& header : spec.headers) {
        if (header.enabled && !header.key.empty()) headers[header.key] = header.value;
    }
    impl.socket.setExtraHeaders(headers);
    if (!spec.subprotocol.empty()) impl.socket.addSubProtocol(spec.subprotocol);
    {
        std::lock_guard lock{impl.mu};
        impl.state = api::WebSocketState::Connecting;
        impl.events.clear();
    }
    Impl* implPtr = impl_.get();
    impl.socket.setOnMessageCallback([implPtr](const ix::WebSocketMessagePtr& message) {
        api::WebSocketEvent event;
        switch (message->type) {
            case ix::WebSocketMessageType::Open:
                implPtr->setState(api::WebSocketState::Connected);
                event.kind = api::WebSocketEventKind::Open;
                event.detail = "已连接";
                break;
            case ix::WebSocketMessageType::Message:
                event.kind = message->binary ? api::WebSocketEventKind::Binary
                                             : api::WebSocketEventKind::Text;
                event.payload = message->str;
                event.wireBytes = message->wireSize;
                break;
            case ix::WebSocketMessageType::Close:
                implPtr->setState(api::WebSocketState::Disconnected);
                event.kind = api::WebSocketEventKind::Close;
                event.closeCode = message->closeInfo.code;
                event.detail = message->closeInfo.reason;
                break;
            case ix::WebSocketMessageType::Error:
                implPtr->setState(api::WebSocketState::Failed);
                event.kind = api::WebSocketEventKind::Error;
                event.detail = message->errorInfo.reason;
                break;
            default:
                return;
        }
        implPtr->push(std::move(event));
    });
    impl.socket.start();
    return {};
}

void WsSession::disconnect() {
    impl_->socket.stop();
    impl_->setState(api::WebSocketState::Disconnected);
}

std::string WsSession::send(const std::string& text, bool binary) {
    if (state() != api::WebSocketState::Connected) return "WebSocket 尚未连接";
    const ix::WebSocketSendInfo info =
        binary ? impl_->socket.sendBinary(text) : impl_->socket.sendText(text);
    return info.success ? std::string{} : "WebSocket 消息发送失败";
}

api::WebSocketState WsSession::state() const {
    std::lock_guard lock{impl_->mu};
    return impl_->state;
}

std::vector<api::WebSocketEvent> WsSession::drain() {
    std::lock_guard lock{impl_->mu};
    return std::exchange(impl_->events, {});
}

} // namespace apitab::ui
