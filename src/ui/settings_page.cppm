// ui/settings_page.cppm — 基础设置弹窗：主题模式。
module;

#include "eui_ui.h"

export module apitab.ui.settings_page;

import std;
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

bool g_themeModeOpen = false;

} // namespace

export void drawSettingsDialog(eui::Ui& ui, const eui::Screen& screen,
                               const AppTheme& theme) {
    if (!g_settingsOpen) return;
    constexpr float kDlgW = 360.0f;
    constexpr float kDlgH = 174.0f;
    components::dialog(ui, "settings.dialog")
        .open(true)
        .screen(screen.width, screen.height)
        .size(kDlgW, kDlgH)
        .title("基础设置")
        .theme(theme.components)
        .content([&] {
            ui.text("settings.theme.label")
                .position(20.0f, 56.0f)
                .size(90.0f, kInputHeight)
                .text("外观主题")
                .fontSize(kFontBody)
                .lineHeight(kInputHeight)
                .color(theme.bodyText)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            ui.stack("settings.theme.wrap")
                .position(112.0f, 56.0f)
                .size(228.0f, kInputHeight)
                .zIndex(20)
                .content([&] {
                    components::dropdown(ui, "settings.theme")
                        .size(228.0f, kInputHeight)
                        .items({"深色模式", "浅色模式", "跟随系统"})
                        .selected(static_cast<int>(g_themeMode))
                        .open(g_themeModeOpen)
                        .theme(theme.components)
                        .onOpenChange([](bool open) { g_themeModeOpen = open; })
                        .onChange([](int selected) {
                            g_themeMode = static_cast<ThemeMode>(std::clamp(selected, 0, 2));
                            if (g_themeMode == ThemeMode::System) g_dark = systemDark();
                            g_themeModeOpen = false;
                        })
                        .build();
                })
                .build();
            ui.text("settings.theme.hint")
                .position(20.0f, 94.0f)
                .size(kDlgW - 40.0f, 28.0f)
                .text("跟随系统会使用当前桌面可用的默认主题。")
                .fontSize(kFontLabel)
                .color(theme.hintText)
                .wrap(true)
                .build();
            components::button(ui, "settings.close")
                .position(kDlgW - 94.0f, 132.0f)
                .size(74.0f, 24.0f)
                .text("关闭")
                .fontSize(kFontLabel)
                .theme(theme.components, false)
                .onClick([] { g_settingsOpen = false; })
                .build();
        })
        .build();
}
