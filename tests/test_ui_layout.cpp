#include <huxerui/testing/ui_test.h>

#include "ui/app.h"
#include "ui/split_pane.h"
#include "ui/page_card.h"
#include "ui/request_doc_section.h"
#include "ui/settings_theme.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>

namespace {
using namespace huxerui;
using namespace huxerui::testing;
using namespace apitab::ui;

std::function<View()> root;
View LayoutTestRoot() { return root(); }

int failures = 0;
void Expect(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        ++failures;
    }
}
bool Near(float actual, float expected) { return std::abs(actual - expected) < 0.1F; }

void CheckSplit(UiTest& ui, Axis axis, float reserved, float available) {
    const auto leading = ui.Find(UiSelector::Key("leading")).One();
    const auto handle = ui.Find(UiSelector::Key("handle")).One();
    const auto trailing = ui.Find(UiSelector::Key("trailing")).One();
    if (axis == Axis::Horizontal) {
        Expect(Near(leading.bounds.x + leading.size.width, handle.bounds.x), "horizontal handle follows leading pane");
        Expect(Near(handle.bounds.x + handle.size.width, trailing.bounds.x), "horizontal panes never overlap");
        Expect(trailing.size.width >= reserved, "editor keeps its minimum width");
        Expect(Near(trailing.bounds.x + trailing.size.width, available), "horizontal split stays inside viewport");
    } else {
        Expect(Near(leading.bounds.y + leading.size.height, handle.bounds.y), "vertical handle follows editor");
        Expect(Near(handle.bounds.y + handle.size.height, trailing.bounds.y), "vertical panes never overlap");
        Expect(leading.size.height >= reserved, "editor keeps its minimum height");
        Expect(Near(trailing.bounds.y + trailing.size.height, available), "vertical split stays inside viewport");
    }
}

void HorizontalResize() {
    const auto metrics = std::make_shared<SplitPaneMetrics>();
    std::optional<State<float>> request;
    root = [&] {
        auto extent = UseState(260.0F);
        request.emplace(extent);
        return SplitPane {
          Column{}.With(Frame{.width = extent.Get(), .min_width = 180.0F}).Key("leading"),
          Column{}.Key("handle"),
          Column{}.With(Frame{.min_width = 480.0F}).Key("trailing"),
        }.LayoutValue<SplitPaneOptions>(SplitPaneOptions{.extent = extent.Get(), .metrics = metrics});
    };
    const Application app{LayoutTestRoot};
    UiTest ui(app, {.viewport = {1100.0F, 690.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 260.0F), "wide window honors requested tree width");
    ui.SetWindowMetrics({.viewport = {720.0F, 540.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 234.0F), "shrinking window clamps tree before editor");
    CheckSplit(ui, Axis::Horizontal, 480.0F, 720.0F);
    ui.SetWindowMetrics({.viewport = {1100.0F, 690.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 260.0F), "expanding window restores preferred width");
    request.value() = 10000.0F;
    ui.Pump();
    Expect(Near(metrics->extent, 420.0F), "tree upper limit is enforced");
    request.value() = 100.0F;
    ui.Pump();
    Expect(Near(metrics->extent, 180.0F), "tree lower limit is enforced");
}

void VerticalResize() {
    const auto metrics = std::make_shared<SplitPaneMetrics>();
    root = [=] {
        return SplitPane {
          Column{}.With(Frame{.min_height = 300.0F}).Key("leading"),
          Column{}.Key("handle"),
          Column{}.With(Frame{.min_height = 140.0F}).Key("trailing"),
        }.LayoutValue<SplitPaneOptions>(SplitPaneOptions{
            .axis = Axis::Vertical, .extent = 640.0F, .minimum = 140.0F, .maximum = 640.0F,
            .reserve = 300.0F, .trailing = true, .metrics = metrics,
        });
    };
    const Application app{LayoutTestRoot};
    UiTest ui(app, {.viewport = {720.0F, 540.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 234.0F), "response leaves 300dp for editor at minimum window height");
    CheckSplit(ui, Axis::Vertical, 300.0F, 540.0F);
    ui.SetWindowMetrics({.viewport = {720.0F, 100.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 0.0F), "undersized host reduces response without negative sizes");
    CheckSplit(ui, Axis::Vertical, 94.0F, 100.0F);
    ui.SetWindowMetrics({.viewport = {720.0F, 1200.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 640.0F), "response restores requested height after expansion");
    ui.SetWindowMetrics({.viewport = {720.0F, 0.0F}});
    ui.Pump();
    // 零视口可跳过布局并保留上一帧几何；恢复后必须重新按父约束限幅。
    ui.SetWindowMetrics({.viewport = {720.0F, 540.0F}});
    ui.Pump();
    Expect(Near(metrics->extent, 234.0F), "restoring a zero-height host recomputes response bounds");
}

void UnboundedScroll() {
    root = [] {
        return ScrollView {
          SplitPane {
            Column{}.With(Frame{.height = 300.0F}).Key("leading"),
            Column{}.Key("handle"),
            Column{}.Key("trailing"),
          }.LayoutValue<SplitPaneOptions>(SplitPaneOptions{
              .axis = Axis::Vertical, .extent = 260.0F, .minimum = 140.0F, .maximum = 640.0F,
              .reserve = 300.0F, .trailing = true,
          }),
        };
    };
    const Application app{LayoutTestRoot};
    UiTest ui(app, {.viewport = {720.0F, 400.0F}});
    ui.Pump();
    const auto response = ui.Find(UiSelector::Key("trailing")).One();
    Expect(Near(response.size.height, 260.0F), "unbounded scroll measures finite preferred response height");
    Expect(Near(response.bounds.y, 306.0F), "unbounded scroll places response after natural editor and handle");
}

void PageCardOutline() {
    for (const bool dark : {false, true}) {
        auto theme = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
        theme.spacing.small = 8.0F;
        theme.shapes.small = 6.0F;
        root = [theme] {
            return PageCardSurface(
                Column{}.With(Grow(1.0F), Background(Color::Rgb(18, 18, 18))).Key("painted-page"),
                theme, Color::Black());
        };
        const Application app{LayoutTestRoot};
        UiTest ui(app, {.viewport = {800.0F, 600.0F}});
        ui.Pump();
        const auto content = ui.Find(UiSelector::Key("painted-page")).One();
        const auto outline = ui.Find(UiSelector::Key("page-card-outline")).One();
        Expect(Near(content.bounds.x, 8.0F) && Near(content.bounds.y, 8.0F),
               "opaque page fills card without extra border padding");
        Expect(Near(content.bounds.x + content.size.width, 792.0F) &&
                   Near(content.bounds.y + content.size.height, 592.0F),
               "page keeps the original outer card margin");
        Expect(content.bounds == outline.bounds, "foreground outline stretches across the same bounds as content");
    }
}

void PageCardMaskGeometry() {
    const auto mask = PageCardCornerMask({800.0F, 600.0F}, 10.0F);
    Expect(mask.Contains({0.25F, 0.25F}, PathFillRule::EvenOdd), "corner mask covers the square top-left corner");
    Expect(mask.Contains({799.75F, 599.75F}, PathFillRule::EvenOdd), "corner mask covers the square bottom-right corner");
    Expect(!mask.Contains({400.0F, 0.25F}, PathFillRule::EvenOdd), "corner mask leaves the straight top edge visible");
    Expect(!mask.Contains({400.0F, 300.0F}, PathFillRule::EvenOdd), "corner mask leaves content visible");
    Expect(!PageCardCornerMask({0.0F, 0.0F}, 10.0F).Contains({0.0F, 0.0F}, PathFillRule::EvenOdd),
           "zero-size card has no corner mask");
}

void DesktopBreakpoints() {
    root = [] {
        return Text(UseViewportClass() == ViewportClass::Compact ? "compact" : "regular");
    };
    const Application app{LayoutTestRoot, {.viewport_breakpoints = kAppViewportBreakpoints}};
    UiTest ui(app, {.viewport = {800.0F, 600.0F}});
    ui.Pump();
    Expect(ui.Find(UiSelector::Text("compact")).Exists(), "minimum desktop width enters compact layout");
    ui.SetWindowMetrics({.viewport = {900.0F, 600.0F}});
    ui.Pump();
    Expect(ui.Find(UiSelector::Text("regular")).Exists(), "900dp restores regular layout");
}

void PageCardFooter() {
    for (const bool dark : {false, true}) {
        auto theme = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
        theme.spacing.small = 8.0F;
        theme.shapes.large = 10.0F;
        root = [theme] {
            return PageCardSurface(
                Column{}.With(Grow(1.0F)).Key("workspace"), theme, Color::Black(),
                Column {
                  ProvideEnvironment(DividerStyle{.color = Color::Black(), .thickness = 0.5F}, Divider()),
                  Row{}.With(Frame{.height = 28.0F}).Key("status"),
                }.With(CrossAlign(CrossAxisAlignment::Stretch)));
        };
        const Application app{LayoutTestRoot};
        UiTest ui(app, {.viewport = {800.0F, 600.0F}});
        ui.Pump();
        const auto workspace = ui.Find(UiSelector::Key("workspace")).One();
        const auto status = ui.Find(UiSelector::Key("status")).One();
        Expect(Near(status.bounds.x, 8.0F) && Near(status.size.width, 784.0F),
               "status bar shares the full card content width");
        Expect(Near(status.bounds.y + status.size.height, 592.0F),
               "status bar sits inside the card above its outer margin");
        Expect(Near(status.bounds.y - workspace.bounds.y - workspace.size.height, 0.5F),
               "one internal separator divides workspace from status bar");
        Expect(Near(status.size.height, 28.0F), "status bar keeps its compact height");
    }
}

void PageCardInteraction() {
    int clicks = 0;
    auto theme = MaterialDarkThemeSpec();
    theme.spacing.small = 8.0F;
    theme.shapes.large = 10.0F;
    root = [&] {
        auto text = UseState(TextEditingValue{"initial"});
        return PageCardSurface(
            Column {
              TextField(text).OnChanged([text](const TextEditingValue& value) { text = value; }).Key("editor"),
              Button("Action").OnClick([&clicks] { ++clicks; }).Key("action"),
              ScrollView {
                Column {
                  Text("first"),
                  Column{}.With(Frame{.height = 1000.0F}),
                  Text("last"),
                }.With(CrossAlign(CrossAxisAlignment::Stretch)),
              }.With(Grow(1.0F)).Key("scroll"),
            }.With(Background(Color::Rgb(18, 18, 18)), CrossAlign(CrossAxisAlignment::Stretch)),
            theme, Color::Black());
    };
    const Application app{LayoutTestRoot};
    UiTest ui(app, {.viewport = {800.0F, 600.0F}});
    ui.Pump();
    ui.Find(UiSelector::Key("action")).Tap();
    Expect(clicks == 1, "foreground outline does not intercept button clicks");
    auto editor = ui.Find(UiSelector::Key("editor"));
    editor.Tap();
    editor.ReplaceText("edited through outline");
    Expect(editor.One().value == "edited through outline", "foreground outline preserves text focus and editing");
    const auto consumed = ui.Find(UiSelector::Key("scroll")).ScrollBy({0.0F, 100.0F});
    Expect(Near(consumed.y, 100.0F), "foreground outline preserves wheel scrolling");
}

void OpenSettingsTheme() {
    for (const bool dark : {false, true}) {
        auto theme = dark ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
        root = [theme] {
            auto open = UseState(false);
            return MaterialTheme(theme, Scope([open]() -> View {
                if (!open.Get())
                    return View{Button("Open settings").OnClick([open] { open = true; })};
                return Scope([open] {
                    const auto& current = UseTheme();
                    auto transition = UseSceneTransition();
                    auto text = UseState(TextEditingValue{"initial"});
                    return SettingsPageTheme(
                        Column {
                          Text("Settings mounted"),
                          TextField(text).Variant(TextFieldVariant::Outlined)
                              .OnChanged([text](const TextEditingValue& value) { text = value; }).Key("settings-input"),
                          Button("Close settings").OnClick([open] { open = false; }),
                        }.With(Grow(1.0F), Background(current.colors.surface),
                               CrossAlign(CrossAxisAlignment::Stretch), transition.Anchor()),
                        current, UseEnvironment<TextFieldStyle>(), UseEnvironment<SelectStyle>(),
                        UseEnvironment<SegmentedButtonStyle>());
                });
            }));
        };
        const Application app{LayoutTestRoot};
        UiTest ui(app, {.viewport = {1180.0F, 760.0F}});
        ui.Pump();
        ui.Find(UiSelector::Text("Open settings")).Tap();
        Expect(ui.Find(UiSelector::Text("Settings mounted")).Exists(), "settings theme mounts after clicking open");
        auto input = ui.Find(UiSelector::Key("settings-input"));
        input.Tap();
        input.ReplaceText("settings remain usable");
        Expect(input.One().value == "settings remain usable", "settings input remains functional inside local theme");
        ui.Find(UiSelector::Text("Close settings")).Tap();
        ui.SetWindowMetrics({.viewport = {800.0F, 600.0F}});
        ui.Pump();
        ui.Find(UiSelector::Text("Open settings")).Tap();
        Expect(ui.Find(UiSelector::Text("Settings mounted")).Exists(), "settings theme reopens at narrow viewport");
    }
}

// 请求文档分区：撰写文档渲染 + 空文档提示。Markdown 可用时挂载 Lib-Markdown，
// 否则走等宽原文降级；两种形态都必须真实挂载、可展开，不只在组合期成立。
void RequestDocSection() {
    auto theme = MaterialDarkThemeSpec();
    for (const std::string doc : {std::string{}, std::string{"# 创建条目\n\n- 需要名称\n- 返回 201\n"}}) {
        root = [theme, doc] {
            return MaterialTheme(theme, Scope([doc]() -> View {
                const auto& current = UseTheme();
                auto cache = UseState(std::make_shared<DocMarkdownCache>()).Get();
                RefreshDocMarkdownCache(*cache, doc);
                return ScrollView {
                    AuthoredDocView(doc, current, *cache),
                }.With(Grow(1.0F));
            }));
        };
        const Application app{LayoutTestRoot};
        UiTest ui(app, {.viewport = {900.0F, 700.0F}});
        ui.Pump();
        if (doc.empty()) {
            Expect(ui.Find(UiSelector::Text(
                       "此请求还没有撰写文档。导入 OpenAPI / Postman 文件时，"
                       "接口的 description 会自动写入这里。"))
                       .Exists(),
                   "empty request doc shows the authoring hint");
        } else {
#if defined(APITAB_HAS_MARKDOWN)
            Expect(ui.Find(UiSelector::Key("authored-doc")).Exists(),
                   "authored doc renders through the Markdown component");
            Expect(ui.Find(UiSelector::Text("创建条目")).Exists(),
                   "Markdown heading becomes visible text");
            Expect(ui.Find(UiSelector::Text("需要名称")).Exists(),
                   "Markdown list item becomes visible text");
#else
            Expect(ui.Find(UiSelector::Text("当前构建未启用 Markdown 渲染，以下为文档原文。"))
                       .Exists(),
                   "doc falls back to plain text when Markdown is unavailable");
#endif
        }
    }
}

// 请求工作区 Dock 骨架（源码通道，镜像 request_page.cpp 的接线）：
// 面板出树只是隐藏（语义树消失），重新入树原样恢复且面板内部状态保留。
#if defined(APITAB_HAS_DOCK_LAYOUT)
void RequestDockWorkspace() {
    auto theme = MaterialDarkThemeSpec();
    std::optional<State<bool>> showResponse;
    root = [&, theme] {
        return MaterialTheme(theme, Scope([&]() -> View {
            const auto& current = UseTheme();
            (void)current;
            auto tree = UseState(DockTree::Split(
                Axis::Horizontal,
                {DockTree::Tabs({"tree"})
                     .With(Frame{.width = 260.0F, .min_width = 180.0F, .max_width = 420.0F}),
                 DockTree::Split(
                     Axis::Vertical,
                     {DockTree::Tabs({"workspace"})
                          .With(Grow(), Frame{.min_width = 480.0F, .min_height = 300.0F}),
                      DockTree::Tabs({"response"})
                          .With(Frame{.height = 260.0F,
                                      .min_height = 140.0F,
                                      .max_height = 640.0F})})}));
            auto visible = UseState(true);
            showResponse.emplace(visible);
            auto editCount = UseState(0);
            // 镜像 request_page：调试开关同步 response 面板在树中的存在性。
            Lifecycle(
                [tree, visible] {
                    DockTree next = tree.Get();
                    if (visible.Get() && !next.Contains("response")) {
                        next = next.Dock("response", "workspace", DockPlacement::Bottom);
                    } else if (!visible.Get() && next.Contains("response")) {
                        next = next.Remove("response");
                    }
                    if (!(next == tree.Get())) tree = std::move(next);
                },
                visible);
            return DockLayout(
                {
                    DockItem("tree", "请求树", Column{Text("tree-panel")}).Closable(false),
                    DockItem("workspace", "编辑器", Column{Text("workspace-panel")})
                        .Closable(false),
                    DockItem("response", "响应区",
                             Column{
                                 Button("Edit")
                                     .OnClick([editCount] { editCount = editCount.Get() + 1; })
                                     .Key("edit"),
                                 Text(std::to_string(editCount.Get()) + " edits")
                                     .Key("edits-label"),
                             })
                        .Closable(false),
                },
                tree.Get())
                .OnChanged([tree](DockTree next) { tree = std::move(next); });
        }));
    };
    const Application app{LayoutTestRoot};
    UiTest ui(app, {.viewport = {1100.0F, 800.0F}});
    ui.Pump();
    Expect(ui.Find(UiSelector::Text("tree-panel")).Exists(), "dock mounts the tree panel");
    Expect(ui.Find(UiSelector::Text("workspace-panel")).Exists(),
           "dock mounts the workspace panel");
    Expect(ui.Find(UiSelector::Text("响应区")).Exists(), "response tab bar is visible");
    ui.Find(UiSelector::Key("edit")).Tap();
    ui.Pump();
    Expect(ui.Find(UiSelector::Text("1 edits")).Exists(), "response panel edits state works");
    showResponse.value() = false;
    ui.Pump();
    ui.Pump(); // Lifecycle 晚一帧应用，再提交一次确认树已收敛
    Expect(!ui.Find(UiSelector::Text("响应区")).Exists(),
           "removing response from tree hides its tab bar and content");
    showResponse.value() = true;
    ui.Pump();
    ui.Pump();
    Expect(ui.Find(UiSelector::Text("响应区")).Exists(), "re-docked response panel reappears");
    ui.Find(UiSelector::Key("edit")).Tap();
    ui.Pump();
    Expect(ui.Find(UiSelector::Text("2 edits")).Exists(),
           "re-docked response panel keeps its mounted state");
}
#endif // APITAB_HAS_DOCK_LAYOUT

} // namespace

int main() {
    HorizontalResize();
    VerticalResize();
    UnboundedScroll();
    DesktopBreakpoints();
    PageCardOutline();
    PageCardMaskGeometry();
    PageCardFooter();
    PageCardInteraction();
    OpenSettingsTheme();
    RequestDocSection();
#if defined(APITAB_HAS_DOCK_LAYOUT)
    RequestDockWorkspace();
#endif
    if (failures == 0) std::puts("UI layout checks passed");
    return failures == 0 ? 0 : 1;
}
