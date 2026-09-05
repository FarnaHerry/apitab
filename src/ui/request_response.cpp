// request_response.cpp — 请求工作区右侧下岛：响应区（Body/Headers/Cookies）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 响应展示），纯搬移、行为零变化。
#include <huxerui/huxerui.h>

#include <string>
#include <cctype>
#include <vector>

#include "ui.h"
#include "syntax_grammars.h"
#include "sweetline_provider.h"

namespace apitab::ui {

namespace {
struct ResponseDocument {
    std::string text;
    std::string syntax;
    std::size_t revision = 0;
    std::shared_ptr<huxerui::codeeditor::EditorDecorationProvider> provider;
};

std::string ResponseSyntax(const std::vector<std::string>& headers) {
    for (auto line : headers) {
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!line.starts_with("content-type:")) continue;
        if (line.find("json") != std::string::npos) return std::string{kJsonSyntax};
        if (line.find("xml") != std::string::npos) return std::string{kXmlSyntax};
    }
    return std::string{kPlainSyntax};
}

[[huxerui::composable]] huxerui::View ResponseBody(
    const std::string& text, const std::vector<std::string>& headers,
    const huxerui::ThemeSpec& theme) {
    auto controller = huxerui::codeeditor::UseEditorController();
    auto searchVisible = huxerui::UseState(false);
    auto query = huxerui::UseState(huxerui::TextEditingValue{});
    auto document = huxerui::UseState(std::make_shared<ResponseDocument>()).Get();
    const auto syntax = ResponseSyntax(headers);
    if (!document->provider || document->text != text || document->syntax != syntax) {
        document->text = text;
        document->syntax = syntax;
        ++document->revision;
        document->provider = std::make_shared<demo::SweetLineDecorationProvider>(
            syntax, text, "response", demo::SweetLineDecorationProvider::GutterIconSource{},
            demo::SweetLineDecorationProvider::PhantomSource{});
    }
    huxerui::codeeditor::EditorOptions options;
    ApplyEditorTypography(options);
    options.read_only = true;
    ConfigureEditorMenu(options);
    options.built_in_search_bar = false;
    options.theme = EditorTheme(theme);
    options.document_key = "response-" + std::to_string(document->revision);
    options.initial_text = text;
    options.sticky_gutter = true;
    options.decoration_providers = {document->provider};
    std::vector<huxerui::View> children;
    if (searchVisible.Get()) {
        children.push_back(huxerui::Row {
          huxerui::TextField(query).Placeholder("查找响应内容")
              .OnChanged([query, controller](const huxerui::TextEditingValue& value) {
                  query = value;
                  controller.RunSearch(value.text);
              }).OnSubmitted([controller] { controller.FindNext(); }).With(huxerui::Grow(1.0F)),
          huxerui::Button("上一处").OnClick([controller] { controller.FindPrevious(); }),
          huxerui::Button("下一处").OnClick([controller] { controller.FindNext(); }),
          huxerui::Button("关闭").OnClick([controller] { controller.ToggleSearch(); }),
        }.With(huxerui::Spacing(theme.spacing.small)));
    } else {
        children.push_back(huxerui::Row {
          huxerui::Text("只读", huxerui::TextRole::Body).With(huxerui::Grow(1.0F)),
          huxerui::Button("查找").OnClick([controller] { controller.ToggleSearch(); }),
        });
    }
    children.push_back(huxerui::codeeditor::CodeEditor(options, controller)
        .On<huxerui::codeeditor::EditorEvents::SearchVisibilityChanged>(
            [searchVisible, query, controller](bool visible) {
                searchVisible = visible;
                if (visible) controller.RunSearch(query.Get().text);
                else controller.ClearSearch();
            }).With(huxerui::Grow(1.0F)));
    // Reapply an active query after the new response document has mounted.
    huxerui::Lifecycle([controller, query, searchVisible] {
        if (searchVisible.Get()) controller.RunSearch(query.Get().text);
    }, document->revision);
    return huxerui::Column(std::move(children)).With(
        huxerui::Spacing(theme.spacing.small),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 传输中的实时视图：SSE 逐条/逐字到达都经 30ms 轮询快照刷新；编辑器是非受控
// 组件（外部改文本须整体 LoadDocument 重载高亮），流式期间用轻量 Text 呈现，
// 完成后才落编辑器拿高亮/搜索。滚动钉底：仅在原本就停在底部时跟随新内容，
// 用户上翻阅读时不打断。
[[huxerui::composable]] huxerui::View LiveResponseStream(
    huxerui::State<std::string> responseBody, const huxerui::ThemeSpec& theme) {
    auto controller = huxerui::UseScrollController();
    huxerui::Lifecycle([controller] {
        const huxerui::ScrollMetrics metrics = controller.Metrics();
        if (metrics.maximum_offset - metrics.offset <= 8.0F)
            controller.ScrollTo(metrics.maximum_offset);
    }, responseBody);
    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(font_size::kMonoBody),
                                  .foreground = theme.colors.on_surface};
    return huxerui::ScrollView {
        huxerui::Text(responseBody.Get(), huxerui::TextRole::Body).Style(mono),
    }
        .Controller(controller)
        .With(huxerui::ScrollBar());
}
} // namespace

// 响应区（右侧下岛）：独立重组作用域 —— responseTab/responseBody/responseHeaders/
// responseCookies 的变化只重组此区域，不扩散到整个编辑器（KV 表不受影响）。
// 标题固定；Body 完成态由编辑器处理滚动，传输中（inFlight）走流式实时视图，
// Headers/Cookies 使用普通 ScrollView。
[[huxerui::composable]] huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                                                   huxerui::State<std::vector<std::string>> responseHeaders,
                                                   huxerui::State<std::vector<std::string>> responseCookies,
                                                   huxerui::State<bool> inFlight,
                                                   const huxerui::ThemeSpec& theme) {
    auto responseTab = huxerui::UseState<std::size_t>(0);

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(font_size::kMonoBody),
                                  .foreground = theme.colors.on_surface};
    huxerui::View content = huxerui::Row{};
    if (responseTab.Get() == 0) {
        content = inFlight.Get()
            ? LiveResponseStream(responseBody, theme)
            : ResponseBody(responseBody.Get(), responseHeaders.Get(), theme);
    } else if (responseTab.Get() == 1) {
        const std::vector<std::string> lines = responseHeaders.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        content = rows.empty()
            ? huxerui::View{huxerui::Text("（无响应头）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(2.0F))}};
    } else {
        const std::vector<std::string> lines = responseCookies.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        content = rows.empty()
            ? huxerui::View{huxerui::Text("（无 Cookie）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(2.0F))}};
    }

    if (responseTab.Get() != 0)
        content = huxerui::ScrollView{content}.With(huxerui::ScrollBar());
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text("响应", huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
            huxerui::SegmentedButton({"Body", "Headers", "Cookies"}, responseTab)
                .OnChanged([responseTab](std::size_t index) { responseTab = index; }),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(content).With(huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
