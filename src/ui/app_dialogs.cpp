// app_dialogs.cpp — 应用级对话框/面板（P1-C2 自 app.cpp 纯搬移）：
//   CloseGuard（关闭询问：直接关闭 / 最小化到托盘 / 取消，托盘可用性动态查询，
//   Hide/Quit 推迟出事件路径）。其它非页面全屏弹层（若后续新增）亦归此文件。
//   本文件不含标题栏/状态条/侧栏/页面路由（见 title_bar.cpp / global_status_bar.cpp / app.cpp）。
#include <huxerui/huxerui.h>

#include <functional>
#include <string>

#include "ui.h"

import apitab.preferences;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View CloseGuard(huxerui::State<int> closeBehavior,
                                                 huxerui::State<bool> closeDialogOpen,
                                                 huxerui::View content) {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    auto dialog = huxerui::UseDialog();
    auto tasks = huxerui::UseTaskScope();

    // 关闭拦截：按配置直接关闭/进托盘；未配置时第一次询问并把选择写入配置。
    // 托盘可用性在关闭时动态查询（不要用组合期快照，避免错过宿主晚就绪）。
    // Hide 一律经 tasks.Launch + Delay(0) 推迟到事件派发之后：在关闭请求回调里
    // 同步隐藏最后一个可见窗口，会让 SDK 运行时在回调栈上就地进行窗口拆毁/
    // 退出路径（回调返回后栈上的 WindowService 已被回收）——用户实测直接崩溃。
    // 先同步 return true 拦下关闭，再在事件循环外 Hide，行为不变、栈安全。
    auto hideToTray = [tasks, window] {
        tasks.Launch([window]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            window.Hide();
        });
    };
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (!tray.IsAvailable()) return false; // 无托盘：交给系统直接关闭
            if (closeBehavior.Get() == 1) return false;
            if (closeBehavior.Get() == 2) {
                hideToTray();
                return true;
            }
            if (closeDialogOpen.Get()) return true;
            closeDialogOpen = true;
            // 自定义内容弹窗（DialogCard 包底板）：内置两按钮弹窗没有取消入口。
            // 三个按钮：直接关闭 / 最小化到托盘 / 取消；按钮点击在弹层指针事件
            // 路径上，Quit/Hide 等全局副作用推迟到事件派发之后（CLAUDE.md 约定 6）。
            dialog.Show(
                [=](huxerui::DialogContext ctx) mutable -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("关闭 apitab？", huxerui::TextRole::Title),
                        huxerui::Text("直接退出应用，还是最小化到系统托盘继续运行？",
                                      huxerui::TextRole::Body),
                        huxerui::Row {
                            huxerui::Button("直接关闭")
                                .OnClick([=]() mutable {
                                    closeDialogOpen = false;
                                    closeBehavior = 1;
                                    saveSessionPreference("close_behavior", "1");
                                    ctx.Dismiss();
                                    tasks.Launch([application]() -> huxerui::Task<void> {
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        application.Quit();
                                    });
                                }),
                            huxerui::Button("最小化到托盘")
                                .OnClick([=]() mutable {
                                    closeDialogOpen = false;
                                    closeBehavior = 2;
                                    saveSessionPreference("close_behavior", "2");
                                    ctx.Dismiss();
                                    hideToTray();
                                }),
                            huxerui::Button("取消").OnClick([=]() mutable {
                                closeDialogOpen = false;
                                ctx.Dismiss();
                            }),
                        }
                            // 两端对齐：与其他弹窗一致（取消类在右、动作在左）。
                            .With(huxerui::Spacing(8.0F),
                                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                          .With(huxerui::Spacing(12.0F),
                                                huxerui::Frame{.width = 360.0F},
                                                huxerui::CrossAlign(
                                                    huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{
                    .dismiss_on_outside_press = false,
                    .dismiss_on_cancel = false,
                });
            return true;
        },
        0);

    return content;
}

// 应用根：状态全部在这里（官方 README 形态：根标注 composable）。

} // namespace apitab::ui
