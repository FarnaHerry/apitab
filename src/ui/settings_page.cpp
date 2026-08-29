// settings_page.cpp — 全局设置：主题模式（跟随系统/深色/浅色）+ 关闭行为。
// 选择都持久化到 settings.ini（会话偏好）。
#include <huxerui/huxerui.h>

#include <string>
#include <vector>

#include "ui.h"

import apitab.preferences;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View GlobalSettingsPage(huxerui::State<int> themeMode,
                                                         huxerui::State<int> closeBehavior) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("全局设置", "外观与应用行为（保存在 settings.ini）"),
        huxerui::Text("主题模式", huxerui::TextRole::Label),
        huxerui::SegmentedButton({"跟随系统", "深色", "浅色"}, themeMode)
            .OnChanged([themeMode, tasks](std::size_t index) {
                // 主题切换会重建整棵组件树：推迟出指针事件路径
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    themeMode = static_cast<int>(index);
                    saveSessionPreference("theme_mode", std::to_string(themeMode.Get()));
                });
            }),
        huxerui::Text("跟随系统会读取当前桌面的深浅色偏好。", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text("关闭行为", huxerui::TextRole::Label),
        huxerui::SegmentedButton({"每次询问", "直接关闭", "最小化到托盘"}, closeBehavior)
            .OnChanged([closeBehavior](std::size_t index) {
                closeBehavior = static_cast<int>(index);
                saveSessionPreference("close_behavior", std::to_string(closeBehavior.Get()));
            }),
        huxerui::Text("点标题栏 ✕ 时生效；“每次询问”在关闭时弹窗确认。", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text("数据目录：~/.local/share/apitab", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))}.With(huxerui::ScrollBar());
}

} // namespace apitab::ui
