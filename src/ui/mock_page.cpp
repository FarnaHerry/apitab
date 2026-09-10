// mock_page.cpp — 请求编辑器子页"Mock"（pageTab=3）：模拟响应定义（状态码/延迟/
// 响应头/响应体），字段一律读写草稿 RequestDraft.mock（draft.h）；落库由调试页
// "保存"按钮经 MockToDb 全量写库，本页面只管草稿。启用后调试页"发送"不发真实
// 请求、直接返回本页定义的模拟响应（拦截在 request_page.cpp 发送协程内）。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <utility>
#include <vector>

#include "ui.h"
#include "draft.h"

namespace apitab::ui {

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
        KvTable(mock.headers, theme, "头名称", "头值", [drafts, index](std::vector<KvRow> rows) {
            MutateDraft(drafts, index,
                        [&](RequestDraft& d) { d.mock.headers = std::move(rows); });
        }).With(huxerui::Grow(1.0F)),
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

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

} // namespace apitab::ui
