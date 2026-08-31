// tcp_session.h — TCP / TCPS 会话：全同步 asio，不拥有任何线程。
// 会话由页面 TaskScope 协程直接持有（State<shared_ptr<TcpSession>>）：
// connect/send/read 是阻塞调用，只准经 RunOnTaskThread 在任务线程上执行；
// 恢复点恒为 UI 线程，状态/事件在那里写 State（见 task_bridge.h 线程契约）。
// close() 可从任意线程调用：shutdown + close 唤醒阻塞中的 read/connect。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

import apitab.api_engine;

namespace apitab::ui {

class TcpSession {
public:
    TcpSession();
    ~TcpSession();

    TcpSession(const TcpSession&) = delete;
    TcpSession& operator=(const TcpSession&) = delete;

    // 阻塞：解析 + 连接（+ TCPS 的 TLS 握手），带 connectTimeoutSec 超时。
    // 返回错误消息或空串。只在任务线程调用。
    std::string connect(const api::TcpSpec& spec);
    // 阻塞写。返回错误消息或空串。只在任务线程调用。
    std::string send(const std::vector<std::uint8_t>& bytes);
    // 阻塞读一条：Received / Disconnected / Error。只在任务线程调用。
    api::TcpEvent read();

    bool isConnected() const;
    // 任意线程：打断阻塞中的 connect/read/send 并关闭连接（幂等）。
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apitab::ui
