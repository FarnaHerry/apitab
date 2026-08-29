// settings_page.cpp — 设置：外观（深色模式，会话持久化）+ 应用信息。
#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"

import apitab.preferences;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View SettingsPage(huxerui::State<bool> dark) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    (void)dark;
    (void)theme;
    return MigrationPlaceholder("设置");
}

} // namespace apitab::ui
