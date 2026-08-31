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

namespace {

// 项目公共头两列 KV 表：语义与 mock_page.cpp 的 MockHeaderTable 一致
// （启用 Checkbox / 键 / 值 / ✕ 删除 / 末行虚拟输入物化为真实行），
// 但那只在 mock_page.cpp 的匿名 namespace 内，此处按本页 State 自写一份。
[[huxerui::composable]] huxerui::View ProjectHeaderTable(
    huxerui::State<std::vector<KvRow>> rows, const huxerui::ThemeSpec& theme) {
    auto tasks = huxerui::UseTaskScope();
    const std::vector<KvRow> data = rows.Get();
    std::vector<huxerui::View> children{
        huxerui::Row {
            huxerui::Text("", huxerui::TextRole::Label)
                .With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text("头名称", huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
            huxerui::Text("头值", huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Foreground(theme.colors.on_surface_variant)),
    };
    for (std::size_t i = 0; i <= data.size(); ++i) {
        const bool phantom = i == data.size();
        const KvRow row = phantom ? KvRow{} : data[i];
        // 行写入：i 越界（虚拟行）时物化新行，否则改写原行。
        auto applyRow = [rows](std::size_t i, KvRow updated) {
            std::vector<KvRow> copy = rows.Get();
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.key.text.empty() && updated.value.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            rows = copy;
        };
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(row.enabled).OnChanged([row, i, applyRow](bool checked) {
                    KvRow updated = row;
                    updated.enabled = checked;
                    applyRow(i, std::move(updated));
                }),
                huxerui::TextField(row.key)
                    .Label("键")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.key = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(row.value)
                    .Label("值")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.value = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                phantom
                    ? huxerui::View{huxerui::Text("", huxerui::TextRole::Label)
                                        .With(huxerui::Padding(4.0F))}
                    : huxerui::View{huxerui::Button("✕").OnClick([tasks, rows, i] {
                        // 删除会卸载本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<KvRow> copy = rows.Get();
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            rows = copy;
                        });
                    })},
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

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
            // ---- 项目说明：多行输入（LineLimits(MultiLine) 回车换行、按词换行）----
            huxerui::Text("项目说明", huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            huxerui::Row {
                huxerui::TextField(description)
                    .Placeholder("这个项目是做什么的（可选）")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .LineLimits(huxerui::TextFieldLineLimits::MultiLine(4))
                    .OnChanged(
                        [description](const huxerui::TextEditingValue& value) {
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
            ProjectHeaderTable(headers, theme),
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
