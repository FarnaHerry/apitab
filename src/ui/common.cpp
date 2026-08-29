// common.cpp — HuxerUI 前端公共小组件（页面标题、状态文本、空状态页）。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <string>

#include "app_resources.h"
#include "ui.h"

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

// 空状态页：图标 + 标题 + 副标题，可选行动按钮（如“去主页打开项目”）。
// navPage 用来在按钮里推迟切页（事件处理器内禁止同步写会卸载节点的 State，
// CLAUDE.md 约定 6），由调用方传入；按钮通过 tasks.Launch + Delay(0) 推迟。
[[huxerui::composable]] huxerui::View EmptyState(huxerui::ImageResource icon,
                                                  std::string title, std::string subtitle,
                                                  huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    return huxerui::Column {
               huxerui::Image(icon)
                   .With(huxerui::Frame{.width = 64.0F, .height = 64.0F},
                         huxerui::Foreground(theme.colors.on_surface_variant)),
               huxerui::Text(std::move(title), huxerui::TextRole::Title),
               huxerui::Text(std::move(subtitle), huxerui::TextRole::Body)
                   .With(huxerui::Foreground(theme.colors.on_surface_variant)),
               huxerui::Button("去主页打开项目").OnClick([tasks, navPage] {
                   tasks.Launch([=]() -> huxerui::Task<void> {
                       co_await huxerui::Delay(std::chrono::duration<double>{0});
                       navPage = std::size_t{0};
                   });
               }),
           }
        .With(huxerui::Padding(theme.spacing.extra_large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::Foreground(theme.colors.on_surface_variant));
}

} // namespace apitab::ui
