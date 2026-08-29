// app.cpp — 应用壳（岛屿架构）：左侧 Logo + 顶级侧边栏；右侧项目标签条 + 内容岛。
// 顶级标签页：第一个为主页（固定不可关），之后每个打开的项目一个标签。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"

import apitab.db;
import apitab.preferences;
import apitab.store.requests;
import apitab.store.loadtest;

namespace apitab::ui {

namespace pages {

enum PageIndex : std::size_t {
    kHome = 0,
    kRequest = 1,
    kLoad = 2,
    kWebSocket = 3,
    kTcp = 4,
    kHistory = 5,
    kSettings = 6,
};

[[huxerui::composable]] huxerui::View PageFor(std::size_t index,
                                              huxerui::State<std::size_t> navPage,
                                              huxerui::State<std::vector<std::int64_t>> tabs,
                                              huxerui::State<std::int64_t> activeProject,
                                              huxerui::State<bool> dark) {
    switch (index) {
        case kHome:
            return HomePage(navPage, tabs, activeProject);
        case kRequest:
            return activeProject.Get() != 0
                       ? RequestPage(activeProject)
                       : MigrationPlaceholder("请求（在项目标签页内使用；先在主页打开项目）");
        case kLoad:
            return LoadTestPage();
        case kWebSocket:
            return WebSocketPage();
        case kTcp:
            return TcpPage();
        case kHistory:
            return HistoryPage();
        default:
            return SettingsPage(dark);
    }
}
} // namespace pages

namespace {

// 软件徽标：字母 AT 合成的圆角块。
[[huxerui::composable]] huxerui::View LogoBadge() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text("AT", huxerui::TextRole::Title)
        .With(huxerui::Frame{.width = 44.0F, .height = 36.0F},
              huxerui::Background(theme.colors.primary),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::Foreground(theme.colors.on_primary),
              huxerui::Align{.horizontal = huxerui::HorizontalAlignment::Center,
                             .vertical = huxerui::VerticalAlignment::Center});
}

// 顶级标签条：第一个为主页（固定不可关），其后每个项目一个可关标签。
[[huxerui::composable]] huxerui::View ProjectTabStrip(
    huxerui::State<std::vector<std::int64_t>> tabs, huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    // 项目 id → 名称（主页标签固定在最前）
    std::vector<std::pair<std::int64_t, std::string>> items;
    items.emplace_back(0, "主页");
    for (const db::Project& p : g_requests.allProjects()) {
        for (std::int64_t id : tabs.Get()) {
            if (id == p.id) items.emplace_back(p.id, p.name);
        }
    }

    auto activateProject = [activeProject](std::int64_t id) {
        if (id != 0) {
            g_requests.selectProject(id);
            g_loadtest.setProject(id);
            saveSessionPreference("active_project", std::to_string(id));
        }
        activeProject = id;
    };

    std::vector<huxerui::View> chips;
    for (const auto& [id, name] : items) {
        const bool active = activeProject.Get() == id;
        const std::string label = (active ? "● " : "") + name;
        chips.push_back(
            huxerui::Row {
                huxerui::Button(label).OnClick([activateProject, id] { activateProject(id); }),
                id != 0 ? huxerui::View{huxerui::Button("✕").OnClick(
                              [tabs, activeProject, activateProject, id] {
                                  // 移除标签；若关闭的是激活标签则回到主页标签
                                  std::vector<std::int64_t> rest = tabs.Get();
                                  std::erase(rest, id);
                                  tabs = rest;
                                  if (activeProject.Get() == id) activateProject(0);
                              })}
                        : huxerui::View{huxerui::Text("")},
            }
                .With(huxerui::Spacing(2.0F)));
    }

    return huxerui::Row(std::move(chips))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 左列岛：Logo + 顶级侧边栏（全部页面按钮）。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        LogoBadge(),
        huxerui::NavigationPane(
            {
                huxerui::NavigationItem("主页"),
                huxerui::NavigationItem("请求"),
                huxerui::NavigationItem("压测"),
                huxerui::NavigationItem("WebSocket"),
                huxerui::NavigationItem("TCP"),
                huxerui::NavigationItem("历史"),
                huxerui::NavigationItem("设置"),
            },
            navPage, true)
            .OnChanged([navPage](std::size_t index) { navPage = index; }),
    }
        .With(huxerui::Padding(theme.spacing.medium), huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Frame{.width = 230.0F});
}

// 应用壳：左列（Logo + 侧边栏岛）｜右列（标签条岛 + 内容岛），岛屿间留缝隙。
[[huxerui::composable]] huxerui::View AppShell(huxerui::State<std::size_t> navPage,
                                               huxerui::State<std::vector<std::int64_t>> tabs,
                                               huxerui::State<std::int64_t> activeProject,
                                               huxerui::State<bool> dark) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        SideShell(navPage),
        huxerui::Column {
            huxerui::Row {ProjectTabStrip(tabs, activeProject)}
                .With(huxerui::Padding(theme.spacing.medium),
                      huxerui::Background(theme.colors.surface_container_low),
                      huxerui::CornerRadius(theme.shapes.large)),
            pages::PageFor(navPage.Get(), navPage, tabs, activeProject, dark)
                .Key(navPage.Get() * 100000 + activeProject.Get()),
        }
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

// 应用根：状态全部在这里（官方 README 形态：根标注 composable）。
[[huxerui::composable]] huxerui::View AppRoot() {
    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    auto dark = huxerui::UseState(sessionPreference("dark") == "1");
    // 顶级标签：activeProject = 0 为主页标签；其余值为打开的项目 id。
    auto tabs = huxerui::UseState<std::vector<std::int64_t>>({});
    auto activeProject = huxerui::UseState<std::int64_t>(0);

    // 会话恢复：标签页列表恢复为标签（不恢复激活上下文，点击标签显式激活）。
    huxerui::Lifecycle(
        [=] -> void {
            const std::string raw = sessionPreference("open_projects");
            if (raw.empty()) return;
            std::vector<std::int64_t> ids;
            std::size_t start = 0;
            while (start <= raw.size()) {
                const std::size_t comma = raw.find(',', start);
                const std::string part =
                    raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                try {
                    if (!part.empty()) ids.push_back(std::stoll(part));
                } catch (...) {
                }
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (!ids.empty()) tabs = ids;
        },
        0);

    return dark.Get()
               ? huxerui::View{huxerui::MaterialDarkTheme{
                     AppShell(navPage, tabs, activeProject, dark)}}
               : huxerui::View{huxerui::MaterialTheme{
                     AppShell(navPage, tabs, activeProject, dark)}};
}

} // namespace apitab::ui
