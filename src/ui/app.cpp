// app.cpp — HuxerUI 前端根：侧栏导航 + 页面切换 + 主题 + 会话恢复。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"

import apitab.preferences;
import apitab.db;
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

[[huxerui::composable]] huxerui::View PageFor(std::size_t index, huxerui::State<bool> dark) {
    switch (index) {
        case kHome:
            return HomePage();
        case kRequest:
            return !sessionPreference("active_project").empty()
                       ? RequestPage()
                       : MigrationPlaceholder("请求（先在主页打开项目）");
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

// 应用根：左侧导航 + 内容区 + 主题。
[[huxerui::composable]] huxerui::View AppRoot() {
    auto selected = huxerui::UseState<std::size_t>(pages::kHome);
    auto dark = huxerui::UseState(sessionPreference("dark") == "1");

    // 会话恢复：进入首个组合时恢复上次打开的项目。
    huxerui::Lifecycle(
        [] -> void {
            const std::string active = sessionPreference("active_project");
            if (active.empty()) return;
            try {
                const std::int64_t projectId = std::stoll(active);
                for (const db::Project& project : g_requests.allProjects()) {
                    if (project.id == projectId) {
                        if (const std::string err =
                                g_requests.selectProjectInOrg(project.orgId, project.id);
                            err.empty()) {
                            g_loadtest.setProject(project.id);
                        }
                        break;
                    }
                }
            } catch (...) {
            }
        },
        0);

    huxerui::View content = huxerui::Row{
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
            selected, true)
            .OnChanged([selected](std::size_t index) { selected = index; }),
        pages::PageFor(selected.Get(), dark).Key(selected.Get())};

    // 主题跟随设置页开关（会话持久化）。
    return dark.Get() ? huxerui::View{huxerui::MaterialDarkTheme{content}}
                      : huxerui::View{huxerui::MaterialTheme{content}};
}

} // namespace apitab::ui
