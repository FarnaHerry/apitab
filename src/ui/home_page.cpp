// home_page.cpp — 主页面：组织 / 项目列表 + 显式打开项目工作区。
// 打开 = selectProjectInOrg（领域状态）+ 记录会话键 + 跳转到请求页；
// 未打开前，依赖项目的页面（请求集合等）一律不可用。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.db;
import apitab.preferences;
import apitab.store.loadtest;
import apitab.store.requests;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View ProjectRow(const db::Project& project,
                                                 huxerui::State<std::size_t> selected,
                                                 huxerui::State<std::int64_t> opened) {
    auto toast = huxerui::UseToast();
    const bool is_open = opened.Get() == project.id;
    return huxerui::Button(is_open ? "进入 — " + project.name : "打开 — " + project.name)
        .OnClick([project, selected, opened, toast] {
            if (const std::string err = g_requests.selectProjectInOrg(
                    g_requests.currentOrgId(), project.id);
                !err.empty()) {
                toast.Show("打开失败: " + err);
                return;
            }
            g_loadtest.setProject(project.id);
            saveSessionPreference("active_project", std::to_string(project.id));
            opened = project.id;
            selected = 1; // 跳转到请求页
        })
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View HomePage(huxerui::State<std::size_t> selected,
                                               huxerui::State<std::int64_t> opened) {
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
        rows.push_back(ProjectRow(project, selected, opened).Key(project.id));
    }
    rows.push_back(
        huxerui::Button("刷新列表").OnClick([refresh] { refresh = refresh.Get() + 1; }));

    return huxerui::ScrollView{
        huxerui::Column(std::move(rows)).With(huxerui::Padding(theme.spacing.large),
                                               huxerui::Spacing(theme.spacing.medium))
    };
}

} // namespace apitab::ui
