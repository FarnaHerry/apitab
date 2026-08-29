// app.cppm — HuxerUI 前端根：侧栏导航 + 页面切换 + 会话恢复。
module;

#include <huxerui/huxerui.h>

export module apitab.ui.app;

import std;
import apitab.i18n;
import apitab.db;
import apitab.store.requests;
import apitab.store.loadtest;
import apitab.store.ui;
import apitab.ui.common;
import apitab.ui.home_page;
import apitab.ui.request_page;

export namespace apitab::ui {

namespace pages {

enum PageIndex : std::size_t { kHome = 0, kRequest = 1, kLoad = 2, kHistory = 3, kSettings = 4 };

[[huxerui::composable]] inline huxerui::View PageFor(std::size_t index) {
    switch (index) {
        case pages::kHome:
            return HomePage();
        case pages::kRequest:
            return g_activeProjectTabId != 0 ? RequestPage()
                                             : MigrationPlaceholder("请求（先在主页打开项目）");
        case pages::kLoad:
            return MigrationPlaceholder("压测");
        case pages::kHistory:
            return MigrationPlaceholder("历史");
        default:
            return MigrationPlaceholder("设置");
    }
}
} // namespace pages

// 应用根：左侧导航 + 内容区。
[[huxerui::composable]] inline huxerui::View AppRoot() {
    auto selected = huxerui::UseState<std::size_t>(pages::kHome);

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
                            g_activeProjectTabId = project.id;
                            g_loadtest.setProject(project.id);
                        }
                        break;
                    }
                }
            } catch (...) {
            }
        },
        0);

    return huxerui::MaterialTheme{huxerui::Row{
        huxerui::NavigationPane(
            {
                huxerui::NavigationItem("主页"),
                huxerui::NavigationItem("请求"),
                huxerui::NavigationItem("压测"),
                huxerui::NavigationItem("历史"),
                huxerui::NavigationItem("设置"),
            },
            selected, true)
            .OnChanged([selected](std::size_t index) { selected = index; }),
        pages::PageFor(selected.Get()).Key(selected.Get())}};
}

} // namespace apitab::ui
