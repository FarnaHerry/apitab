// websocket_engine.cpp — IXWebSocket 实现；回调只投递事件并唤醒 UI。
module;

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace core::platform { void requestUiUpdate(); }

module apitab.websocket_engine;

import std;
import apitab.api_engine;

namespace {

class IxWebSocketEngine final : public api::WebSocketEngine {
public:
    IxWebSocketEngine() { ix::initNetSystem(); }
    ~IxWebSocketEngine() override {
        shuttingDown_.store(true);
        disconnect();
        ix::uninitNetSystem();
    }

    std::string connect(const api::WebSocketSpec& spec) override {
        if (!spec.url.starts_with("ws://") && !spec.url.starts_with("wss://")) {
            return "WebSocket 地址必须以 ws:// 或 wss:// 开头";
        }
        disconnect();
        const std::uint64_t generation = ++generation_;
        {
            std::lock_guard lock(stateMutex_);
            state_ = api::WebSocketState::Connecting;
        }
        socket_ = std::make_unique<ix::WebSocket>();
        socket_->disableAutomaticReconnection();
        socket_->setUrl(spec.url);
        socket_->setHandshakeTimeout(std::max(1, spec.handshakeTimeoutSec));
        ix::WebSocketHttpHeaders headers;
        for (const auto& header : spec.headers) {
            if (header.enabled && !header.key.empty()) headers[header.key] = header.value;
        }
        socket_->setExtraHeaders(headers);
        if (!spec.subprotocol.empty()) socket_->addSubProtocol(spec.subprotocol);
        socket_->setOnMessageCallback([this, generation](const ix::WebSocketMessagePtr& message) {
            if (shuttingDown_.load() || generation != generation_.load()) return;
            api::WebSocketEvent event;
            switch (message->type) {
                case ix::WebSocketMessageType::Open:
                    setState(api::WebSocketState::Connected);
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
                    setState(api::WebSocketState::Disconnected);
                    event.kind = api::WebSocketEventKind::Close;
                    event.closeCode = message->closeInfo.code;
                    event.detail = message->closeInfo.reason;
                    break;
                case ix::WebSocketMessageType::Error:
                    setState(api::WebSocketState::Failed);
                    event.kind = api::WebSocketEventKind::Error;
                    event.detail = message->errorInfo.reason;
                    break;
                default:
                    return;
            }
            {
                std::lock_guard lock(eventsMutex_);
                if (events_.size() >= kMaxEvents) events_.erase(events_.begin());
                events_.push_back(std::move(event));
            }
            core::platform::requestUiUpdate();
        });
        socket_->start();
        return {};
    }

    void disconnect() override {
        ++generation_;
        if (socket_) {
            socket_->stop();
            socket_.reset();
        }
        setState(api::WebSocketState::Disconnected);
    }

    std::string sendText(const std::string& text, bool binary) override {
        if (!socket_ || state() != api::WebSocketState::Connected) return "WebSocket 尚未连接";
        const ix::WebSocketSendInfo info = binary ? socket_->sendBinary(text) : socket_->sendText(text);
        return info.success ? std::string{} : "WebSocket 消息发送失败";
    }

    api::WebSocketState state() const override {
        std::lock_guard lock(stateMutex_);
        return state_;
    }

    std::vector<api::WebSocketEvent> drainEvents() override {
        std::lock_guard lock(eventsMutex_);
        return std::exchange(events_, {});
    }

private:
    void setState(api::WebSocketState state) {
        std::lock_guard lock(stateMutex_);
        state_ = state;
    }

    static constexpr std::size_t kMaxEvents = 2000;
    std::unique_ptr<ix::WebSocket> socket_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> shuttingDown_{false};
    mutable std::mutex stateMutex_;
    api::WebSocketState state_ = api::WebSocketState::Disconnected;
    std::mutex eventsMutex_;
    std::vector<api::WebSocketEvent> events_;
};

} // namespace

std::unique_ptr<api::WebSocketEngine> makeWebSocketEngine() {
    return std::make_unique<IxWebSocketEngine>();
}
