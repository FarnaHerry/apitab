// app_main.cpp — apitab 入口：HuxerUI Application + RunApplication。
// UI 内容在 src/ui/*.cpp（composable 普通源，经 huxerui_add_app 的 codegen 处理）。
#include <huxerui/huxerui.h>

#include "ui/app.h"

import apitab.preferences;

// EUI 时代由框架提供的 UI 唤醒钩子。HuxerUI 是 State 驱动失效模型，
// 引擎结果由 TaskScope 的轮询任务在 UI 线程取回并写 State，这里降级为 no-op。
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
            .title_bar_height = 44.0F,
        }},
};

int main() {
    loadSessionPreferences();
    return huxerui::RunApplication();
}
