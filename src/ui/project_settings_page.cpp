// project_settings_page.cpp — 当前项目设置：名称 / 说明 / 公共请求头，统一「保存」落库。
// 未选择项目的兜底已删：主页整宽覆盖侧栏后，未打开项目时本页不可达。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import apitab.db;
import apitab.api_engine;
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
    auto description = huxerui::UseState(huxerui::TextEditingValue{project->description});
    // 公共头草稿：api::KeyValue → KvRow 逐字段搬（初始值在 UseState 前算好）。
    std::vector<KvRow> initial_headers;
    for (const api::KeyValue& h : project->headers) {
        initial_headers.push_back(KvRow{huxerui::TextEditingValue{h.key},
                                        huxerui::TextEditingValue{h.value},
                                        huxerui::TextEditingValue{h.type},
                                        huxerui::TextEditingValue{h.remark}, h.enabled});
    }
    auto headers = huxerui::UseState(std::move(initial_headers));

    // 岛屿分区模型：普通表单控件固定在顶部，公共请求头表自身使用 VirtualList。
    // 不再给 VirtualList 套外层 ScrollView，避免无界内容约束和嵌套滚动竞争。
    return huxerui::Column {
        PageHeader("项目设置", "当前项目：" + project->name),
        huxerui::Row {
            huxerui::TextField(name)
                .Label("项目名称")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([name](const huxerui::TextEditingValue& value) { name = value; })
                .With(huxerui::Grow(1.0F)),
        },
        // ---- 项目说明：多行输入（LineLimits(MultiLine) 回车换行、按词换行）----
        huxerui::Text("项目说明", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Row {
            huxerui::TextField(description)
                .Placeholder("这个项目是做什么的（可选）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .LineLimits(huxerui::TextFieldLineLimits::MultiLine(4))
                .OnChanged([description](const huxerui::TextEditingValue& value) {
                    description = value;
                })
                .With(huxerui::Grow(1.0F)),
        },
        // ---- 项目公共请求头 ----
        huxerui::Text("项目公共请求头", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text(
            "本项目的每个请求发送时自动带上；请求里显式写了同名头（大小写不敏感）"
            "则不覆盖；环境变量 {{var}} 不参与公共头替换。",
            huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        KvTable(headers.Get(), theme, "头名称", "头值",
                [headers](std::vector<KvRow> values) { headers = std::move(values); })
            .With(huxerui::Grow(1.0F)),
        huxerui::Row {
            huxerui::Button("保存").OnClick(
                [name, description, headers, toast, current] {
                    if (name.Get().text.empty()) {
                        toast.Show("项目名称不能为空");
                        return;
                    }
                    // KvRow → api::KeyValue 手转（字段序 key/value/enabled/type/remark）
                    std::vector<api::KeyValue> kvs;
                    for (const KvRow& r : headers.Get()) {
                        kvs.push_back(api::KeyValue{r.key.text, r.value.text, r.enabled,
                                                    r.type.text, r.remark.text});
                    }
                    if (const std::string err = g_requests.updateProjectMeta(
                            current, name.Get().text, description.Get().text, kvs);
                        !err.empty())
                        toast.Show("保存失败: " + err);
                    else
                        toast.Show("已保存");
                }),
        },
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::Frame{.min_width = 320.0F, .min_height = 240.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
