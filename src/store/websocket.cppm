// store/websocket.cppm — WebSocket 会话 store：引擎不暴露给 UI。
export module apitab.store.websocket;

import std;
import apitab.api_engine;
import apitab.websocket_engine;

export class WebSocketStore {
public:
    WebSocketStore() : engine_(makeWebSocketEngine()) {}

    std::string connect(std::int64_t tabUid, const api::WebSocketSpec& spec) {
        if (ownerTabUid_ != 0 && ownerTabUid_ != tabUid) engine_->disconnect();
        ownerTabUid_ = tabUid;
        return engine_->connect(spec);
    }

    void disconnect(std::int64_t tabUid) {
        if (ownerTabUid_ != tabUid) return;
        engine_->disconnect();
        ownerTabUid_ = 0;
    }

    std::string send(std::int64_t tabUid, const std::string& text, bool binary) {
        if (ownerTabUid_ != tabUid) return "WebSocket 会话不属于当前标签";
        return engine_->sendText(text, binary);
    }

    api::WebSocketState state(std::int64_t tabUid) const {
        return ownerTabUid_ == tabUid ? engine_->state() : api::WebSocketState::Disconnected;
    }

    std::vector<api::WebSocketEvent> drain(std::int64_t tabUid) {
        if (ownerTabUid_ != tabUid) return {};
        return engine_->drainEvents();
    }

    void release(std::int64_t tabUid) { disconnect(tabUid); }

private:
    std::unique_ptr<api::WebSocketEngine> engine_;
    std::int64_t ownerTabUid_ = 0;
};

export WebSocketStore g_websocket;
