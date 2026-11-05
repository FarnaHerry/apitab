#pragma once

// request_doc_section.h — 请求页「文档」分区撰写版的纯渲染（Markdown）。
//
// 从 request_doc.cpp 抽出，便于 UI 测试直接挂载验证。这里**不调用任何组合 hook**
// （UseState/UseToast）：缓存由调用方持有，链接回调由调用方注入——composable 一律
// 留在 .cpp，hcg 才会重写它（头文件里的 `[[huxerui::composable]]` 会被编译器忽略）。
//
// Markdown 解析按 doc 原文与缓存比对，只有原文变化才重新解析；解析失败保留错误
// 信息并降级显示原文。未启用 Lib-Markdown 的构建整体降级为等宽原文。

#include <huxerui/huxerui.h>

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#if defined(APITAB_HAS_MARKDOWN)
#include <huxerui/markdown.h>
#endif

namespace apitab::ui {

// 撰写文档的渲染缓存；由持有 State 的 composable 长期保存。
struct DocMarkdownCache {
    std::string source;
    std::string error;  // 非空 = 解析失败，退化显示原文
#if defined(APITAB_HAS_MARKDOWN)
    huxerui::markdown::MarkdownDocument document;
#endif
};

// 空文档提示（两种构建形态共用）。
inline huxerui::View AuthoredDocEmptyHint(const huxerui::ThemeSpec& theme) {
    return huxerui::Text("此请求还没有撰写文档。导入 OpenAPI / Postman 文件时，"
                         "接口的 description 会自动写入这里。",
                         huxerui::TextRole::Body)
        .With(huxerui::Foreground(theme.colors.on_surface_variant));
}

// 解析日期原文；只在 source 变化时执行。异常转成缓存里的 error，不外抛。
inline void RefreshDocMarkdownCache(DocMarkdownCache& cache, const std::string& doc) {
    if (cache.source == doc) return;
    cache.source = doc;
    cache.error.clear();
#if defined(APITAB_HAS_MARKDOWN)
    try {
        cache.document = huxerui::markdown::ParseMarkdown(doc);
    } catch (const std::exception& ex) {
        cache.error = ex.what();
    }
#else
    (void)doc;
#endif
}

// 渲染撰写文档。onLink 可为空：链接只提示、不自动导航由调用方决定。
[[nodiscard]] inline huxerui::View AuthoredDocView(
    const std::string& doc, const huxerui::ThemeSpec& theme, const DocMarkdownCache& cache,
    std::function<void(std::string_view)> onLink = {}) {
    if (doc.empty()) return AuthoredDocEmptyHint(theme);

#if defined(APITAB_HAS_MARKDOWN)
    if (!cache.error.empty()) {
        return huxerui::Column {
            huxerui::Text("文档渲染失败：" + cache.error, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.error)),
            huxerui::Text(doc, huxerui::TextRole::Body)
                .Style(huxerui::TextStyle{
                    .font = huxerui::Font::Monospace(14.0F),
                    .foreground = theme.colors.on_surface}),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key("authored-doc");
    }
    huxerui::markdown::MarkdownOptions options;
    options.style = huxerui::markdown::MarkdownStyle::FromTheme(theme);
    huxerui::View rendered = huxerui::markdown::Markdown(cache.document, options);
    if (onLink) {
        rendered =
            std::move(rendered)
                .On<huxerui::markdown::MarkdownEvents::LinkActivated>(
                    [onLink = std::move(onLink)](
                        const huxerui::markdown::MarkdownLinkActivation& event) {
                        onLink(event.destination);
                    });
    }
    return std::move(rendered).Key("authored-doc");
#else
    (void)cache;
    (void)onLink;
    return huxerui::Column {
        huxerui::Text("当前构建未启用 Markdown 渲染，以下为文档原文。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text(doc, huxerui::TextRole::Body)
            .Style(huxerui::TextStyle{
                .font = huxerui::Font::Monospace(14.0F),
                .foreground = theme.colors.on_surface}),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
        .Key("authored-doc");
#endif
}

} // namespace apitab::ui
