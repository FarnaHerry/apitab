// 引擎轮询协程（HuxerUI TaskScope 结构化并发）。
//
// State 只在 UI 线程读写；引擎结果通过 PollWhile 按节拍取回。
// 阻塞或 CPU 密集型工作使用 HuxerUI RunWorker；其协程恢复点在 UI 线程。
#pragma once

#include <huxerui/huxerui.h>

#include <chrono>
#include <functional>

namespace apitab::ui {

// 每 interval 在 UI 线程执行一次 tick()；tick 返回 true 继续等待，false 结束。
inline huxerui::Task<void> PollWhile(std::chrono::duration<double> interval,
                                     std::function<bool()> tick) {
    while (tick()) co_await huxerui::Delay(interval);
}

} // namespace apitab::ui
