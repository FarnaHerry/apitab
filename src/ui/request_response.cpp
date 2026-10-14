// request_response.cpp — 请求工作区右侧下岛：响应区（Body/Headers/Cookies/Preview）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 响应展示）；Preview 档为 HTML 响应
// 预览（Lib-WebView，Linux = WebKitGTK 6.0，APITAB_HAS_WEBVIEW 门控）。
#include <huxerui/huxerui.h>

#ifdef APITAB_HAS_WEBVIEW
#include <huxerui/webview.h>
#endif

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

std::string ResponseSyntax(const huxerui::StateList<std::string>& headers) {
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

// Preview 档可用性：content-type 声明 HTML（text/html、application/xhtml+xml）
// 即可预览；响应没给 content-type 时按正文嗅探（浏览器同款降级）。
bool IsHtmlResponse(const huxerui::StateList<std::string>& headers, const std::string& body) {
    for (auto line : headers) {
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (!line.starts_with("content-type:")) continue;
        return line.find("html") != std::string::npos;
    }
    std::size_t start = 0;
    while (start < body.size() &&
           std::isspace(static_cast<unsigned char>(body[start])))
        ++start;
    std::string sniff;
    sniff.reserve(std::min<std::size_t>(body.size() - start, 32));
    for (std::size_t i = start; i < body.size() && sniff.size() < 32; ++i)
        sniff.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(body[i]))));
    return sniff.starts_with("<!doctype html") || sniff.starts_with("<html");
}

[[huxerui::composable]] huxerui::View ResponseBody(
    const std::string& text, huxerui::StateList<std::string> headers,
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

#ifdef APITAB_HAS_WEBVIEW
// HTML 响应预览：原生 WebView（Lib-WebView；Linux = WebKitGTK，应用级
// InstallWebView 钩子注册的适配器）。内容不作为受控属性——挂载后与每次新响应
// （revision 变化）由 Lifecycle 重放 LoadHtml（与上方 CodeEditor 的搜索重放同
// 一挂载时序契约）；主框架跳转（链接点击/JS 重定向）一律取消，预览恒锚定本次
// 响应，子框架与子资源不受影响，相对路径经 base_url 解析。JS 默认开启：页面
// 脚本参与渲染是 HTML 预览的常态。
[[huxerui::composable]] huxerui::View ResponsePreviewPane(const ResponsePreview& doc) {
    auto controller = huxerui::UseWebViewController();
    huxerui::Lifecycle([controller, doc] {
        controller.LoadHtml({.content = doc.html, .base_url = doc.base_url});
    }, doc.revision);
    return huxerui::WebView({.url = "about:blank", .java_script_enabled = true},
                            controller)
        .On<huxerui::WebViewEvents::NavigationRequested>(
            [](const huxerui::WebViewNavigationRequest& request) {
                return !request.is_main_frame;
            })
        .With(huxerui::Grow(1.0F), huxerui::ClipChildren());
}

// Preview 档内容（含占位分流）。hcg codegen 不支持 composable 函数体内的条件
// 编译，WebView 相关分支整体收在这两个整函数级 #ifdef 里，ResponseArea 只经
// 它们接入。
[[huxerui::composable]] huxerui::View ResponsePreviewTabContent(
    huxerui::State<ResponsePreview> responsePreview,
    huxerui::StateList<std::string> responseHeaders,
    huxerui::State<bool> inFlight) {
    if (inFlight.Get())
        return huxerui::View{huxerui::Text("接收中…", huxerui::TextRole::Body)};
    const ResponsePreview doc = responsePreview.Get();
    if (doc.html.empty())
        return huxerui::View{huxerui::Text("尚未收到可预览的响应", huxerui::TextRole::Body)};
    if (!IsHtmlResponse(responseHeaders, doc.html))
        return huxerui::View{huxerui::Text("当前响应不是 HTML，无法预览", huxerui::TextRole::Body)};
    return ResponsePreviewPane(doc);
}

void AppendResponsePreviewTab(std::vector<huxerui::StringVariant>& tabs) {
    tabs.emplace_back("Preview");
}
#else
// 无 WebView 构建：Preview 档不存在（AppendResponsePreviewTab 不追加标签，
// SegmentedButton 只有三个选项），下方空桩仅为满足链接、运行期不可达。
huxerui::View ResponsePreviewTabContent(huxerui::State<ResponsePreview>,
                                        huxerui::StateList<std::string>,
                                        huxerui::State<bool>) {
    return huxerui::Row{};
}

void AppendResponsePreviewTab(std::vector<huxerui::StringVariant>&) {}
#endif
} // namespace

// 响应区（右侧下岛）：独立重组作用域 —— responseTab/responseBody/responseHeaders/
// responseCookies/responsePreview 的变化只重组此区域，不扩散到整个编辑器（KV 表
// 不受影响）。标题固定；Body 完成态由编辑器处理滚动，传输中（inFlight）走流式
// 实时视图，Headers/Cookies 使用 VirtualList，并由 StateList 直接驱动可视区；
// Preview 档仅在 APITAB_HAS_WEBVIEW 编译期存在，非 HTML 响应给占位说明。
[[huxerui::composable]] huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                                                   huxerui::StateList<std::string> responseHeaders,
                                                   huxerui::StateList<std::string> responseCookies,
                                                   huxerui::State<ResponsePreview> responsePreview,
                                                   huxerui::State<bool> inFlight,
                                                   const huxerui::ThemeSpec& theme) {
    auto responseTab = huxerui::UseState<std::size_t>(0);

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(font_size::kMonoBody),
                                  .foreground = theme.colors.on_surface};
    huxerui::View content = huxerui::Row{};
    if (responseTab.Get() == 0) {
        content = inFlight.Get()
            ? LiveResponseStream(responseBody, theme)
            : ResponseBody(responseBody.Get(), responseHeaders, theme);
    } else if (responseTab.Get() == 1) {
        content = responseHeaders.Empty()
            ? huxerui::View{huxerui::Text("（无响应头）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{
                  huxerui::VirtualList(responseHeaders, [mono](const std::string& line) {
                      return huxerui::Text(line, huxerui::TextRole::Body).Style(mono);
                  })
                      .EstimatedItemExtent(20.0F)
                      .CacheExtent(120.0F)
                      .With(huxerui::ScrollBar(),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}};
    } else if (responseTab.Get() == 2) {
        content = responseCookies.Empty()
            ? huxerui::View{huxerui::Text("（无 Cookie）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{
                  huxerui::VirtualList(responseCookies, [mono](const std::string& line) {
                      return huxerui::Text(line, huxerui::TextRole::Body).Style(mono);
                  })
                      .EstimatedItemExtent(20.0F)
                      .CacheExtent(120.0F)
                      .With(huxerui::ScrollBar(),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}};
    } else {
        // Preview 档：仅 APITAB_HAS_WEBVIEW 构建会追加该标签（无 WebView 构建
        // 里 SegmentedButton 只有三个选项，本分支不可达）；WebView 随档挂载/
        // 卸载（PlatformView 是重资源，不常驻），占位态给文字说明。
        content = ResponsePreviewTabContent(responsePreview, responseHeaders, inFlight);
    }
    std::vector<huxerui::StringVariant> tabs{"Body", "Headers", "Cookies"};
    AppendResponsePreviewTab(tabs);
    return huxerui::Column {
        huxerui::Row {
            huxerui::Text("响应", huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
            huxerui::SegmentedButton(tabs, responseTab)
                .OnChanged([responseTab](std::size_t index) { responseTab = index; }),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        std::move(content).With(huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
