// request_doc.cpp — 请求文档页（只读，按当前草稿自动生成）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 文档展示），纯搬移。
#include <huxerui/huxerui.h>

#include <format>
#include <string>
#include <vector>

#include "draft.h"
#include "ui.h"

import apitab.store.requests;

namespace apitab::ui {

// 文档页：按当前请求草稿自动生成接口文档（只读；草稿任何编辑触发重组即刷新）。
[[huxerui::composable]] huxerui::View RequestDocPage(const RequestDraft& snapshot,
                                                     const std::string& envBaseUrl) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string method = std::string{kMethodNames[snapshot.methodIndex]};
    // 组合 URL 与发送逻辑同规则：输入自带 URI scheme 原样，否则拼当前环境 baseUrl。
    const std::string fullUrl =
        g_requests.composeUrl(snapshot.url.text, 0, g_requests.currentEnvId());

    std::vector<huxerui::View> sections;
    sections.push_back(huxerui::Text(DraftDisplayName(snapshot), huxerui::TextRole::Title));
    sections.push_back(
        huxerui::Row {
            huxerui::Text(method, huxerui::TextRole::Label)
                .Style(huxerui::TextStyle{
                    .font = huxerui::Font::Monospace(font_size::kCaption)
                                .WithWeight(huxerui::FontWeight::SemiBold),
                    .foreground = MethodColor(theme, method)}),
            huxerui::Text(fullUrl.empty() ? "（未填写 URL）" : fullUrl,
                          huxerui::TextRole::Body),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    // KV 节：只列启用且有名目的条目；空节显示"无"。
    const auto kvSection = [&theme, &sections](const std::string& title,
                                               const std::vector<KvRow>& rows) {
        sections.push_back(huxerui::Text(title, huxerui::TextRole::Label)
                               .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        bool any = false;
        for (const KvRow& row : rows) {
            if (!row.enabled || row.key.text.empty()) continue;
            any = true;
            sections.push_back(huxerui::Row {
                huxerui::Text(row.key.text, huxerui::TextRole::Body)
                    .With(huxerui::Frame{.min_width = 120.0F}),
                huxerui::Text(row.value.text, huxerui::TextRole::Body)
                    .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            }
                .With(huxerui::Spacing(theme.spacing.small)));
        }
        if (!any)
            sections.push_back(huxerui::Text("无", huxerui::TextRole::Body)
                                   .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    };
    kvSection("请求参数", snapshot.params);
    kvSection("请求头", snapshot.headers);
    kvSection("Cookies", snapshot.cookies);

    // 请求体：Form 类按字段表，文本类展示原文（等宽），"无"则整节跳过。
    if (snapshot.bodyKindIndex == 3 || snapshot.bodyKindIndex == 4) {
        kvSection(std::format("请求体（{}）", kBodyTypeNames[snapshot.bodyKindIndex]),
                  snapshot.bodyFields);
    } else if (snapshot.bodyKindIndex != 0) {
        sections.push_back(
            huxerui::Text(
                std::format("请求体（{}）", kBodyTypeNames[snapshot.bodyKindIndex]),
                huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        const std::string& body = snapshot.bodies[snapshot.bodyKindIndex].text;
        sections.push_back(
            huxerui::Text(body.empty() ? "（空）" : body, huxerui::TextRole::Body)
                .Style(huxerui::TextStyle{
                    .font = huxerui::Font::Monospace(font_size::kMonoBody),
                    .foreground = theme.colors.on_surface}));
    }

    return huxerui::ScrollView{huxerui::Column(std::move(sections))
                                   .With(huxerui::Spacing(theme.spacing.small),
                                         huxerui::CrossAlign(
                                             huxerui::CrossAxisAlignment::Stretch))}
        .With(huxerui::ScrollBar(), huxerui::Grow(1.0F));
}

} // namespace apitab::ui
