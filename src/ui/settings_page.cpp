// settings_page.cpp — 设置：外观（深色模式，会话持久化）+ 应用信息。
#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"

import apitab.preferences;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View SettingsPage(huxerui::State<bool> dark) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("设置", "外观与应用偏好"),
        huxerui::Switch("深色主题", dark).OnChanged([dark](bool value) {
            dark = value;
            saveSessionPreference("dark", value ? "1" : "0");
        }),
        huxerui::Text("主题跟随此开关立即生效；偏好保存在 settings.ini。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text("数据目录：~/.local/share/apitab", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))};
}

} // namespace apitab::ui
