// project_settings_page.cpp — 当前项目设置：项目名称重命名（不展示内部 ID）。
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

    // 岛屿分区模型：岛本身占满整个页面区块（Grow + Stretch），内容在岛内部滚动。
    // 输入框/保存按钮套 Row：岛交叉轴 Stretch 会拉满直接子节点，Row 主轴不拉伸
    // 子节点；名称输入框在 Row 内用 Grow 占满行宽。
    return huxerui::Column {
        PageHeader("项目设置", "当前项目：" + project->name),
        huxerui::ScrollView{huxerui::Column {
            huxerui::Row {
                huxerui::TextField(name)
                    .Label("项目名称")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([name](const huxerui::TextEditingValue& value) { name = value; })
                    .With(huxerui::Grow(1.0F)),
            },
            huxerui::Row {
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
            },
        }
                                .With(huxerui::Spacing(theme.spacing.medium))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
