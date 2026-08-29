// project_settings_page.cpp — 当前项目设置：项目信息与重命名。
// 未选择项目的兜底已删：主页整宽覆盖侧栏后，未打开项目时本页不可达。
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

    const std::int64_t current = g_requests.currentProjectId();
    const db::Project* project = nullptr;
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == current) project = &p;
    }
    if (project == nullptr) {
        // 防御：正常路径不可达（主页遮盖侧栏）。
        return huxerui::Text("未打开项目", huxerui::TextRole::Body)
            .With(huxerui::Padding(theme.spacing.large),
                  huxerui::Foreground(theme.colors.on_surface_variant));
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
                                     huxerui::Spacing(theme.spacing.medium),
                                     huxerui::Background(theme.colors.surface_container_low),
                                     huxerui::CornerRadius(theme.shapes.large))}.With(huxerui::ScrollBar());
}

} // namespace apitab::ui
