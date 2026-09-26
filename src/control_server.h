// control_server.h — 控制面**服务端半边**（在 GUI 进程里跑）。
//
// 依赖 huxerui（ApplicationContext / TaskScope::Post），所以与客户端半边
// （control.h，可单独编测）分开。形态与理由见 docs/plans/runtime-control-surface.md。
#pragma once

#include <huxerui/app.h>

#include <functional>
#include <mutex>
#include <string>

#include "control.h"

namespace apitab::control {

// 应用线程投递口：控制面线程 → 应用线程的唯一通道。
// 应用安装期还没有组合作用域（拿不到 TaskScope::Post），所以由根组合在挂载时注入
// `TaskScope::Post`（见 src/ui/app.cpp），卸载时清空。线程安全：Set/Clear 来自
// 应用线程，Post 来自控制面线程。
class ApplicationPoster {
public:
    using Post = std::function<void(std::function<void()>)>;

    void Set(Post post);
    void Clear();
    // 投递一条命令；尚未注入（或已随组合卸载）→ false，调用方回 503。
    bool PostTask(std::function<void()> task) const;

private:
    mutable std::mutex mutex_;
    Post post_;
};

// 应用安装钩子（AppOptions::application_hooks）：起控制面服务。
// 起不来只打印一行 stderr，不拖累 GUI——agent 侧会看到"实例未运行/端点不可达"。
void InstallControlServer(huxerui::ApplicationContext& context);

} // namespace apitab::control
