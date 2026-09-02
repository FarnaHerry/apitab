// mock_page.cpp — 请求编辑器子页"Mock"（pageTab=3）：模拟响应定义（状态码/延迟/
// 响应头/响应体），字段一律读写草稿 RequestDraft.mock（draft.h）；落库由调试页
// "保存"按钮经 MockToDb 全量写库，本页面只管草稿。启用后调试页"发送"不发真实
// 请求、直接返回本页定义的模拟响应（拦截在 request_page.cpp 发送协程内）。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include "ui.h"
#include "draft.h"

namespace apitab::ui {

namespace {

// Mock 响应头表：KvTable 的两列精简版（键/值 + 启用勾选），语义一致——
// 末尾恒渲染一个虚拟空行，对它写入键或值即物化为真实行（仅聚焦/移动光标
// 触发的空 OnChanged 不追加）；✕ 只给真实行，删除会卸载本按钮所在行，
// 推迟出指针事件路径（CLAUDE.md 约定 6）。KvRow 的 type/remark 不使用。
[[huxerui::composable]] huxerui::View MockHeaderTable(
    std::vector<KvRow> rows, const huxerui::ThemeSpec& theme,
    std::function<void(std::vector<KvRow>)> onChanged) {
    auto tasks = huxerui::UseTaskScope();
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
    for (std::size_t i = 0; i <= rows.size(); ++i) {
        const bool phantom = i == rows.size();
        const KvRow row = phantom ? KvRow{} : rows[i];
        // 行写入：i 越界（虚拟行）时物化新行，否则改写原行。
        auto applyRow = [rows, onChanged](std::size_t i, KvRow updated) {
            std::vector<KvRow> copy = rows;
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.key.text.empty() && updated.value.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            onChanged(std::move(copy));
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
                    ? huxerui::View{huxerui::Row{}.With(
                          huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                    : AppIconButton("✕", "删除此行", [tasks, rows, i, onChanged] {
                        // 删除会卸载本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<KvRow> copy = rows;
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            onChanged(std::move(copy));
                        });
                    }, AppIconButtonShape::Bare),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]] huxerui::View MockPage(RequestDraft snapshot,
                                               huxerui::State<std::vector<RequestDraft>> drafts,
                                               std::size_t index) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const MockDraft& mock = snapshot.mock;

    std::vector<huxerui::View> children{
        // 顶部行：启用开关 + 一句浅色说明。
        huxerui::Row {
            huxerui::Checkbox("启用 Mock", mock.enabled)
                .OnChanged([drafts, index](bool checked) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.mock.enabled = checked; });
                }),
            huxerui::Text("启用后，调试页『发送』不发真实请求，直接返回以下模拟响应",
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant),
                      huxerui::Grow(1.0F), huxerui::ClipChildren()),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        // 数字字段文本承载（受控 TextField）：空/非法在发送与落库时按默认值换算。
        huxerui::Row {
            huxerui::TextField(mock.status)
                .Label("状态码")
                .Placeholder("200")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.mock.status = value; });
                })
                .With(huxerui::Grow(1.0F)),
            huxerui::TextField(mock.delayMs)
                .Label("延迟 (ms)")
                .Placeholder("0")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.mock.delayMs = value; });
                })
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text("响应头", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        MockHeaderTable(mock.headers, theme, [drafts, index](std::vector<KvRow> rows) {
            MutateDraft(drafts, index,
                        [&](RequestDraft& d) { d.mock.headers = std::move(rows); });
        }),
        huxerui::Text("响应体", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        // 多行编辑区：LineLimits(MultiLine) 即多行语义（回车换行、按词换行、
        // 顶对齐），最少 8 行、随内容长高，由本页面外层 ScrollView 统一滚动。
        huxerui::TextField(mock.body)
            .Placeholder("模拟响应正文（纯文本，可多行）")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(8))
            .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                MutateDraft(drafts, index,
                            [&](RequestDraft& d) { d.mock.body = value; });
            }),
        huxerui::Text("改动需点「保存」落库；启用 Mock 时调试页「发送」即返回此模拟响应。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    };

    return huxerui::ScrollView{huxerui::Column(std::move(children))
                                   .With(huxerui::Spacing(theme.spacing.small),
                                         huxerui::CrossAlign(
                                             huxerui::CrossAxisAlignment::Stretch))}
        .With(huxerui::ScrollBar(), huxerui::Grow(1.0F));
}

} // namespace apitab::ui
