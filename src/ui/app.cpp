// app.cpp — HuxerUI 前端根：侧栏导航 + 页面切换 + 会话恢复。
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

enum PageIndex : std::size_t { kHome = 0, kRequest = 1, kLoad = 2, kHistory = 3, kSettings = 4 };

[[huxerui::composable]] huxerui::View PageFor(std::size_t index) {
    switch (index) {
        case pages::kHome:
            return HomePage();
        case pages::kRequest:
            return !sessionPreference("active_project").empty() ? RequestPage()
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
[[huxerui::composable]] huxerui::View AppRoot() {
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
                            saveSessionPreference("active_project", std::to_string(project.id));
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
