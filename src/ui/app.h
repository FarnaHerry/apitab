// app.h — apitab 应用根声明（定义在 app.cpp，HuxerUI composable 函数）。
#pragma once

#include <huxerui/huxerui.h>

#include <optional>

namespace apitab::ui {

// 托盘窗口控制器：应用级共享状态，托盘激活处理器（应用生命周期注册一次）经它
// 定位目标窗口。WindowHandle 由根组合在挂载时写入、卸载时清除。
// 读写都在应用线程（组合与托盘回调同线程），不需要加锁。
class TrayWindowController {
public:
    void SetWindow(huxerui::WindowHandle window) { window_.emplace(window); }
    void ClearWindow() { window_.reset(); }

    void Activate() const {
        if (window_) window_->Activate();
    }
    // 轻量模式用：隐藏窗口（停止出帧，实测 CPU → 0），不销毁、不退出应用。
    void Hide() const {
        if (window_) window_->Hide();
    }
    bool HasWindow() const { return window_.has_value(); }

private:
    std::optional<huxerui::WindowHandle> window_;
};

// 应用安装钩子（AppOptions::application_hooks）：注册托盘激活处理器。
// 必须走应用安装而不是组合体——SystemTrayHandle::OnActivate 是**应用级一次性
// 注册**（活到 Runtime 关闭，重复注册抛 std::logic_error），放进会重组的
// AppRoot 后第二次重组就会抛异常终止进程（HuxerUI 08acc36「separate application
// and window ownership」重构后的托盘契约：旧 API 自己包 Lifecycle + 支持依赖
// 重连，新 API 只认一次注册；见 third_party/huxerui/docs/design/system-tray.md
// 与 docs/guide/core-concepts.md）。处理器不捕获窗口，改经 TrayWindowController
// 定位——该控制器也在这里 Provide 给根组合。
void InstallSystemTray(huxerui::ApplicationContext& context);

// 进程级窗口控制器：应用安装钩子里创建并在这里发布，根组合写入当前窗口句柄。
// 控制面命令（如 lightweight）在应用线程上经它操作窗口。
void PublishWindowController(TrayWindowController* controller);
TrayWindowController* CurrentWindowController();

// 应用根：侧栏导航 + 页面切换。由 src/app.cpp 注册到 Application。
huxerui::View AppRoot();

} // namespace apitab::ui
