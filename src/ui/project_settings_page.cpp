// project_settings_page.cpp — 当前项目设置：项目信息与重命名。
// 未打开项目时显示空状态（去主页打开项目）。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import apitab.db;
import apitab.store.requests;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View ProjectSettingsPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto navPage = huxerui::UseState<std::size_t>(0);

    const std::int64_t current = g_requests.currentProjectId();
    const db::Project* project = nullptr;
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == current) project = &p;
    }
    if (project == nullptr) {
        return huxerui::ScrollView{EmptyState(app::images::project_settings, "项目设置未打开",
                                              "先在主页打开一个项目，再进入项目设置。", navPage)}
            .With(huxerui::ScrollBar());
    }

    auto name = huxerui::UseState(huxerui::TextEditingValue{project->name});

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("项目设置", "当前项目：" + project->name),
        huxerui::Text("项目 ID: " + std::to_string(project->id), huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text("组织 ID: " + std::to_string(project->orgId), huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::TextField(name)
            .Label("项目名称")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([name](const huxerui::TextEditingValue& value) { name = value; }),
        huxerui::Button("保存名称").OnClick([name, toast, current] {
            if (name.Get().text.empty()) {
                toast.Show("项目名称不能为空");
                return;
            }
            if (const std::string err = g_requests.renameProject(current, name.Get().text);
                !err.empty())
                toast.Show("保存失败: " + err);
            else
                toast.Show("已保存");
        }),
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))}.With(huxerui::ScrollBar());
}

} // namespace apitab::ui
