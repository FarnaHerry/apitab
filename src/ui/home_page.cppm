// home_page.cppm — 主页面：组织 / 项目列表 + 打开项目工作区。
// 数据来自领域 store（g_requests），打开项目 = selectProjectInOrg + g_activeProjectTabId。
module;

#include <huxerui/huxerui.h>

export module apitab.ui.home_page;

import std;
import apitab.db;
import apitab.store.loadtest;
import apitab.store.requests;
import apitab.store.ui;
import apitab.ui.common;

export namespace apitab::ui {

[[huxerui::composable]] inline huxerui::View ProjectRow(const db::Project& project) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Button("打开 — " + project.name)
        .OnClick([project] {
            if (const std::string err = g_requests.selectProjectInOrg(
                    g_requests.currentOrgId(), project.id);
                err.empty()) {
                g_activeProjectTabId = project.id;
                g_loadtest.setProject(project.id);
            }
        })
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    (void)theme;
}

[[huxerui::composable]] inline huxerui::View HomePage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto refresh = huxerui::UseState(0);

    // 数据快照：组合期读领域 store（单线程 UI，重组合时重新拉取）。
    const std::int64_t refreshKey = refresh.Get();
    (void)refreshKey;
    const std::vector<db::Project> projects = g_requests.projects();

    std::vector<huxerui::View> rows;
    rows.push_back(PageHeader("apitab", "API 测试与压测 — 选择一个项目进入工作区"));
    if (projects.empty()) {
        rows.push_back(huxerui::Text("暂无项目", huxerui::TextRole::Body));
    }
    for (const db::Project& project : projects) {
        rows.push_back(ProjectRow(project));
    }
    rows.push_back(
        huxerui::Button("刷新列表").OnClick([refresh] { refresh = refresh.Get() + 1; }));

    return huxerui::ScrollView{
        huxerui::Column(std::move(rows)).With(huxerui::Padding(theme.spacing.large),
                                               huxerui::Spacing(theme.spacing.medium))
    };
}

} // namespace apitab::ui
