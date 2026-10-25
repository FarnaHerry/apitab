#include <huxerui/testing/ui_test.h>

#include "ui/app.h"
#include "ui/split_pane.h"
#include "ui/page_card.h"

#include <cmath>
#include <cstdio>
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
} // namespace

int main() {
    HorizontalResize();
    VerticalResize();
    UnboundedScroll();
    DesktopBreakpoints();
    PageCardOutline();
    PageCardFooter();
    PageCardInteraction();
    if (failures == 0) std::puts("UI layout checks passed");
    return failures == 0 ? 0 : 1;
}
