// store/tcp.cppm — 原始 TCP / TCPS 会话 store：引擎不暴露给 UI。
export module apitab.store.tcp;

import std;
import apitab.api_engine;
import apitab.tcp_engine;

export class TcpStore {
public:
    TcpStore() : engine_(makeTcpEngine()) {}

    std::string connect(std::int64_t tabUid, const api::TcpSpec& spec) {
        if (ownerTabUid_ != 0 && ownerTabUid_ != tabUid) engine_->disconnect();
        ownerTabUid_ = tabUid;
        return engine_->connect(spec);
    }

    void disconnect(std::int64_t tabUid) {
        if (ownerTabUid_ != tabUid) return;
        engine_->disconnect();
        ownerTabUid_ = 0;
    }

    std::string send(std::int64_t tabUid, const std::vector<std::uint8_t>& bytes) {
        if (ownerTabUid_ != tabUid) return "TCP 会话不属于当前标签";
        return engine_->send(bytes);
    }

    api::TcpState state(std::int64_t tabUid) const {
        return ownerTabUid_ == tabUid ? engine_->state() : api::TcpState::Disconnected;
    }

    std::vector<api::TcpEvent> drain(std::int64_t tabUid) {
        if (ownerTabUid_ != tabUid) return {};
        return engine_->drainEvents();
    }

    void release(std::int64_t tabUid) { disconnect(tabUid); }

private:
    std::unique_ptr<api::TcpEngine> engine_;
    std::int64_t ownerTabUid_ = 0;
};

export TcpStore g_tcp;
