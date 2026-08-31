// ws_session.h — WebSocket 会话：IXWebSocket 自管内部线程，应用侧不拥有线程。
// 会话由页面 TaskScope 协程直接持有（State<shared_ptr<WsSession>>）：connect/
// disconnect/send/drain 全在 UI 线程调用；IX 回调（IX 线程）只把事件推入内部
// 队列，UI 侧 PollWhile 泵按节拍 drain 后写 State（见 task_bridge.h 线程契约）。
#pragma once

#include <memory>
#include <string>
#include <vector>

import apitab.api_engine;

namespace apitab::ui {

class WsSession {
public:
    WsSession();
    ~WsSession();

    WsSession(const WsSession&) = delete;
    WsSession& operator=(const WsSession&) = delete;

    // 立即返回（握手在 IX 线程上异步进行）；结果经 drain 出 Open/Error 事件。
    std::string connect(const api::WebSocketSpec& spec);
    void disconnect();
    std::string send(const std::string& text, bool binary);
    api::WebSocketState state() const;
    // UI 线程泵：取走累计的事件（一次取空）。
    std::vector<api::WebSocketEvent> drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apitab::ui
