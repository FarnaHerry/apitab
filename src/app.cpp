// app.cpp — apitab 应用声明：HuxerUI Application 单例。
// 平台入口 main() 在 platform/<platform>/main.cpp（HuxerUI CLI 生成格式）；
// UI 内容在 src/ui/*.cpp（composable 普通源，经 huxerui_add_app 的 codegen 处理）。
#include <huxerui/huxerui.h>

#include "ui/app.h"

// EUI 时代由框架提供的 UI 唤醒钩子。HuxerUI 是 State 驱动失效模型，
// 引擎结果由 TaskScope 的轮询任务在 UI 线程取回并写 State，这里降级为 no-op。
// 引擎实现单元（curl/k6/websocket/tcp_engine.cpp）经全局模块片段前向声明引用本符号。
namespace core::platform {
void requestUiUpdate() {}
} // namespace core::platform

const huxerui::Application application{
    apitab::ui::AppRoot,
    huxerui::AppOptions{
        .window = {
            .title = "apitab — API 测试与压测",
            .initial_size = {1180.0F, 760.0F},
            .chrome_mode = huxerui::WindowChromeMode::Custom,
            // 自定义标题栏的期望逻辑高度（SDK 会保留更大的系统控件最小值）。
            // 内容高 24pt： tabs/齿轮均 Frame height=24。
            .title_bar_height = 24.0F,
        }},
};
