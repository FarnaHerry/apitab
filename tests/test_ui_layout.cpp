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
        Expect(Near(content.bounds.x, 9.0F) && Near(content.bounds.y, 9.0F),
               "opaque page leaves top and left card outline uncovered");
        Expect(Near(content.bounds.x + content.size.width, 791.0F) &&
                   Near(content.bounds.y + content.size.height, 591.0F),
               "opaque page leaves right and bottom card outline uncovered");
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
} // namespace

int main() {
    HorizontalResize();
    VerticalResize();
    UnboundedScroll();
    DesktopBreakpoints();
    PageCardOutline();
    if (failures == 0) std::puts("UI layout checks passed");
    return failures == 0 ? 0 : 1;
}
