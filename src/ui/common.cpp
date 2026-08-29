// common.cpp — HuxerUI 前端公共小组件（页面标题、状态文本、占位页）。
#include <huxerui/huxerui.h>

import std;

namespace apitab::ui {

// 页面标题 + 副标题。
[[huxerui::composable]] huxerui::View PageHeader(std::string title, std::string subtitle) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
               huxerui::Text(std::move(title), huxerui::TextRole::Title),
               huxerui::Text(std::move(subtitle), huxerui::TextRole::Body),
           }
        .With(huxerui::Spacing(4.0F), huxerui::Foreground(theme.colors.on_surface_variant));
}

// 尚未迁移完成的页面占位。
[[huxerui::composable]] huxerui::View MigrationPlaceholder(std::string pageName) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
               huxerui::Text(std::move(pageName) + " — 迁移中", huxerui::TextRole::Title),
               huxerui::Text("此页面正在从 EUI-NEO 前端迁移到 HuxerUI，功能暂不可用。",
                             huxerui::TextRole::Body),
           }
        .With(huxerui::Padding(theme.spacing.large), huxerui::Spacing(8.0F),
              huxerui::Foreground(theme.colors.on_surface_variant));
}

} // namespace apitab::ui
