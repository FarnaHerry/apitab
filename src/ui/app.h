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
    void SetApplication(huxerui::ApplicationHandle application) { application_.emplace(application); }
    void SetWindow(huxerui::WindowHandle window) {
        window_.emplace(window);
        retired_ = false;
    }
    void ClearWindow() { window_.reset(); }

    void Activate() const {
        if (window_) window_->Activate();
    }
    // CloseGuard 的"最小化到托盘"用：隐藏窗口（停止出帧），不销毁、不退出应用。
    void Hide() const {
        if (window_) window_->Hide();
    }
    // 轻量模式：请求退役主窗口（fork 会级联退役 owned 子窗口），应用无窗常驻，
    // 渲染内存随窗口销毁由框架释放。退役在当前回调返回后异步执行，retired_
    // 先行置位——窗口期内的唤回请求立即走重建路径，不会撞上 retired 句柄。
    void Retire() {
        if (window_) {
            retired_ = true;
            window_->Retire();
        }
    }
    bool HasWindow() const { return window_.has_value() && !retired_; }

    // 唤回主窗口（托盘激活、二次启动 show、退出轻量统一入口）：窗口仍在时仅
    // Show+Activate；已退役（轻量模式）时 OpenWindow 重建——重建窗口走同一
    // AppRoot 路由（key 空 = 主窗口），首组合经 Lifecycle 把新句柄写回本控制器，
    // 界面状态按会话持久化恢复（"热重启 UI"，Clash-Flux 托盘运行时同款语义）。
    // 定义在 app.cpp：主窗口 WindowOptions 与 src/app.cpp 的 AppOptions.window
    // 同值（title/尺寸/chrome），两处需同步修改。
    void ShowWindow();

private:
    std::optional<huxerui::ApplicationHandle> application_;
    std::optional<huxerui::WindowHandle> window_;
    // InstallSystemTray 时还没有窗口；首组合挂载（SetWindow）前置 true 与
    // "窗口尚未挂载"的防御语义一致。
    bool retired_ = true;
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
// 多窗口：初始窗口走完整装配；子窗口（WindowIdentity.key = "project:<id>"）
// 在 AppRoot 入口分流到 ChildWindowRoot（见 app.cpp）。
huxerui::View AppRoot();

// 子窗口根：拆出的项目工作区窗口（外观与主窗口一致，行为路由见 app.cpp 注释）。
huxerui::View ChildWindowRoot(const huxerui::WindowIdentity& identity);

} // namespace apitab::ui
