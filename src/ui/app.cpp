// app.cpp — 应用壳（统一内容卡片 + 自定义标题栏 + 托盘）：
//   标题栏直接使用窗口底色：Logo + 顶级标签条（TopTabStrip：主页钉在最左、
//     项目标签横向滚动、设置单例标签固定追加在所有项目标签之后）+ 齿轮（全局设置
//     单例标签）+ 用户头像 + 框架窗口按钮；期望高度 24dp，系统按钮可提高下限。
//     主题为黑白灰极简风，青色品牌色用于主操作、焦点提示和明确的交互状态。
//   下方：左侧图标侧边栏（使用窗口底色）｜统一 PageCard 内容区（卡片负责大区域
//   的底色与外框，页面内部结构用单条细线分隔）。根节点刷整窗底色（rootSpec.colors.background——
//   AppRoot 在主题 provider 之上，UseTheme 只能拿到默认浅色 spec，须按 dark 自选）。
//   顶级位置模型（island-structure-theme.md §13.1，P1-B0.1）：主页/项目/全局设置
//   统一为同一套顶级标签（TopTabId/TopTabState 见 ui.h），内容区按 activeTopTab
//   切换；navPage 只表达项目工作区内部页，不再包含 kHome/kAppSettings 等顶级目的地。
//   响应式：UseViewportClass() Compact 时收窄侧栏宽度与各处间距。
// 托盘：托盘图标/菜单（显示主窗口/退出）；关闭行为三选（每次询问/直接关闭/
//   最小化到托盘），未配置时第一次关闭弹窗询问并把选择写入配置。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "page_card.h"
#include "control_server.h"
#include "lightweight.h"
#include "window_manager.h"
#include "app_resources.h"

import apitab.config;
import apitab.db;
import apitab.preferences;
import apitab.store.requests;
import apitab.store.loadtest;
import apitab.utils;

namespace apitab::ui {

namespace pages {

// 项目工作区内部页（navPage 只表达项目工作区内的页面；顶级目的地由 activeTopTab
// 表达——island-structure-theme.md §13.1，P1-B0.1）。旧 kHome/kAppSettings 已删除
//（不再有双状态竞争路径），kWebSocket/kTcp 本就不可达（已并入请求页内部标签）
// 一并清理。
enum PageIndex : std::size_t {
    kRequest = 0,
    kLoad = 1,
    kHistory = 2,
    kProjectSettings = 3,
};

[[huxerui::composable]] huxerui::View PageCard(huxerui::View content, bool withProjectStatus = false) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    if (withProjectStatus) {
        huxerui::DividerStyle dividerStyle = huxerui::UseEnvironment<huxerui::DividerStyle>();
        dividerStyle.color = islands.line_default;
        dividerStyle.thickness = kLineWidth;
        huxerui::View footer = huxerui::Column {
          huxerui::ProvideEnvironment(dividerStyle, huxerui::Divider()),
          GlobalStatusBar(),
        }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
        return PageCardSurface(content, theme, islands.line_default, footer);
    }
    return PageCardSurface(content, theme, islands.line_default);
}

[[huxerui::composable]] huxerui::View ProjectPages(
    huxerui::State<std::size_t> selectedPage,
    huxerui::State<std::int64_t> activeProject,
    WindowId windowId) {
    std::vector<huxerui::View> pages;
    pages.reserve(4);
    pages.push_back(RequestPage(activeProject, windowId)
                        .Key(std::size_t{kRequest})
                        .With(huxerui::Grow(1.0F)));
    pages.push_back(LoadTestPage()
                        .Key(std::size_t{kLoad})
                        .With(huxerui::Grow(1.0F)));
    pages.push_back(HistoryPage()
                        .Key(std::size_t{kHistory})
                        .With(huxerui::Grow(1.0F)));
    pages.push_back(ProjectSettingsPage()
                        .Key(std::size_t{kProjectSettings})
                        .With(huxerui::Grow(1.0F)));
    return huxerui::IndexedPages(std::move(pages), selectedPage.Get());
}
} // namespace pages

namespace {

// 标题栏几何常量与 TopTabDisplayKey 已移至 ui.h / title_bar.cpp（P1-C2 纯搬移）：
// kTitleBarContentHeight / kProjectTabWidth / kSettingsTabDisplayKey 见 ui.h；
// TopTabDisplayKey / ProjectTabDragPayload / TopTab / TopTabStrip / TitleBarLogo 见 title_bar.cpp。

// apitab 极简主题：深色模式采用黑白灰阶，品牌青留给按钮、焦点等交互状态。
huxerui::ThemeSpec MinimalDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 26.0F,
    };
    spec.shapes = huxerui::ShapeScheme{.extra_small = 3.0F,
                                       .small = 6.0F,
                                       .medium = 6.0F,
                                       .large = 10.0F,
                                       .extra_large = 12.0F,
                                       .full = 10000.0F};
    spec.spacing = huxerui::SpacingScheme{.extra_small = 4.0F,
                                          .small = 8.0F,
                                          .medium = 12.0F,
                                          .large = 16.0F,
                                          .extra_large = 24.0F};
    spec.motion.fast = 0.16;
    spec.motion.normal = 0.20;
    spec.motion.slow = 0.30;
    // 深色：画布纯黑、正文白色、次级文字与结构线用灰色；品牌青只用于按钮、焦点等
    // 明确的交互状态。表面层级保持中性灰，避免大面积区域带蓝/青色偏。
    spec.colors.primary = huxerui::Color::Rgb(67, 211, 220);  // #43D3DC（品牌按钮/焦点色）
    spec.colors.on_primary = huxerui::Color::Black();
    spec.colors.primary_container = huxerui::Color::Rgb(32, 32, 32);
    spec.colors.on_primary_container = huxerui::Color::White();
    spec.colors.secondary = huxerui::Color::Rgb(176, 176, 176);  // #b0b0b0
    spec.colors.on_secondary = huxerui::Color::Black();
    spec.colors.secondary_container = huxerui::Color::Rgb(48, 48, 48);  // #303030
    spec.colors.on_secondary_container = huxerui::Color::White();
    spec.colors.tertiary_container = huxerui::Color::Rgb(42, 42, 42);  // #2a2a2a
    spec.colors.on_tertiary_container = huxerui::Color::White();
    spec.colors.background = huxerui::Color::Black();
    spec.colors.surface = huxerui::Color::Black();
    spec.colors.surface_container_low = huxerui::Color::Rgb(18, 18, 18);  // #121212：低层表面
    spec.colors.surface_container = huxerui::Color::Rgb(32, 32, 32);      // #202020：控件底
    spec.colors.surface_container_high = huxerui::Color::Rgb(42, 42, 42); // #2a2a2a：悬停表面
    spec.colors.surface_container_highest = huxerui::Color::Rgb(54, 54, 54); // #363636：浮层底
    spec.colors.on_surface = huxerui::Color::White();
    spec.colors.on_surface_variant = huxerui::Color::Rgb(176, 176, 176);  // #b0b0b0
    spec.colors.outline = huxerui::Color::Rgb(102, 102, 102);  // #666666
    spec.colors.inverse_surface = huxerui::Color::White();
    spec.colors.inverse_on_surface = huxerui::Color::Black();
    spec.colors.scrim = huxerui::Color::Rgb(0, 0, 0, 0.65F);
    spec.colors.error = huxerui::Color::Rgb(248, 81, 73);             // danger #f85149
    spec.interactions.focus_ring =
        huxerui::FocusRing{spec.colors.primary, kEmphasisLineWidth, 2.0F};
    return spec;
}

huxerui::ThemeSpec MinimalLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 26.0F,
    };
    spec.shapes = huxerui::ShapeScheme{.extra_small = 3.0F,
                                       .small = 6.0F,
                                       .medium = 6.0F,
                                       .large = 10.0F,
                                       .extra_large = 12.0F,
                                       .full = 10000.0F};
    spec.spacing = huxerui::SpacingScheme{.extra_small = 4.0F,
                                          .small = 8.0F,
                                          .medium = 12.0F,
                                          .large = 16.0F,
                                          .extra_large = 24.0F};
    spec.motion.fast = 0.16;
    spec.motion.normal = 0.20;
    spec.motion.slow = 0.30;
    // 浅色：GitHub light 的中性骨架（canvas 纯白 #ffffff / subtle #f6f8fa /
    // 边线 #d0d7de / 字 #1f2328 / 次级 #59636e），分区与画布同色由线划分；
    // 品牌青 #28B8C7 保留为主题色（不用 GitHub 蓝）。
    spec.colors.primary = huxerui::Color::Rgb(40, 184, 199);          // #28B8C7（品牌主色）
    spec.colors.on_primary = huxerui::Color::Rgb(6, 51, 58);
    spec.colors.primary_container = huxerui::Color::Rgb(234, 238, 242);
    spec.colors.on_primary_container = huxerui::Color::Rgb(31, 35, 40);
    spec.colors.secondary = huxerui::Color::Rgb(89, 99, 110);
    spec.colors.on_secondary = huxerui::Color::White();
    spec.colors.secondary_container = huxerui::Color::Rgb(215, 221, 227);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(31, 35, 40);
    spec.colors.tertiary_container = huxerui::Color::Rgb(246, 248, 250);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(31, 35, 40);
    spec.colors.background = huxerui::Color::White();                 // canvas #ffffff
    spec.colors.surface = huxerui::Color::White();
    spec.colors.surface_container_low = huxerui::Color::Rgb(246, 248, 250); // #f6f8fa：行 hover / canvas.subtle
    spec.colors.surface_container = huxerui::Color::Rgb(234, 238, 242);     // #eaeef2：行选中 / 按钮底
    spec.colors.surface_container_high = huxerui::Color::Rgb(225, 230, 235);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(215, 221, 227); // 浮层底
    spec.colors.on_surface = huxerui::Color::Rgb(31, 35, 40);         // #1f2328
    spec.colors.on_surface_variant = huxerui::Color::Rgb(89, 99, 110); // #59636e
    spec.colors.outline = huxerui::Color::Rgb(208, 215, 222);         // #d0d7de
    spec.colors.inverse_surface = huxerui::Color::Rgb(31, 35, 40);
    spec.colors.inverse_on_surface = huxerui::Color::White();
    spec.colors.scrim = huxerui::Color::Rgb(31, 35, 40, 0.45F);
    spec.colors.error = huxerui::Color::Rgb(207, 34, 46);             // danger #cf222e
    spec.interactions.focus_ring =
        huxerui::FocusRing{spec.colors.primary, kEmphasisLineWidth, 2.0F};
    return spec;
}

// 组件样式从当前 Material 值复制，再覆盖品牌需要的字段，保留未自定义的行为。
[[huxerui::composable]] huxerui::View MinimalThemeStyles(huxerui::View content) {
    const huxerui::ThemeSpec& spec = huxerui::UseTheme();
    const IslandTheme lines = ResolveIslandTheme(spec);
    huxerui::ThemeDefinition definition;
    const auto withAlpha = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };

    // 按钮：GitHub neutral 语言——中性灰底 + 正常字色（非反色实底），
    // hover 用高灰阶档位（深色上提亮、浅色上加深，方向由色阶定义表达）。
    huxerui::ButtonStyle buttons = huxerui::UseEnvironment<huxerui::ButtonStyle>();
    buttons.background = spec.colors.surface_container;
    buttons.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                             spec.colors.on_surface};
    buttons.disabled_background = withAlpha(spec.colors.on_surface, 0.10F);
    buttons.disabled_label = withAlpha(spec.colors.on_surface, 0.42F);
    buttons.padding = huxerui::EdgeInsets::Symmetric(16.0F, 9.0F);
    buttons.minimum_height = 36.0F;
    buttons.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    buttons.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = spec.colors.surface_container_high},
        .press = huxerui::IndicationLayer{
            .fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    definition.Set(buttons);

    huxerui::IconButtonStyle icons = huxerui::UseEnvironment<huxerui::IconButtonStyle>();
    icons.foreground = spec.colors.on_surface_variant;
    icons.disabled_foreground = withAlpha(spec.colors.on_surface, 0.38F);
    icons.icon_size = 16.0F;
    icons.minimum_interactive_size = 40.0F;
    icons.state_layer_size = 32.0F;
    icons.corner_radius = spec.shapes.small;
    icons.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    definition.Set(icons);

    huxerui::SegmentedButtonStyle segments =
        huxerui::UseEnvironment<huxerui::SegmentedButtonStyle>();
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.surface_container_highest;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_surface;
    segments.border = huxerui::Border{lines.line_subtle, kLineWidth};
    segments.selected_border = huxerui::Border{lines.line_focus, kLineWidth};
    segments.padding = huxerui::EdgeInsets::Symmetric(12.0F, 6.0F);
    segments.minimum_height = 32.0F;
    segments.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    definition.Set(segments);

    huxerui::ChipStyle chips = huxerui::UseEnvironment<huxerui::ChipStyle>();
    chips.background = spec.colors.surface_container_low;
    chips.selected_background = spec.colors.surface_container_highest;
    chips.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kChip),
                                           spec.colors.on_surface_variant};
    chips.selected_label = spec.colors.on_surface;
    chips.border = huxerui::Border{lines.line_subtle, kLineWidth};
    chips.selected_border = huxerui::Border{lines.line_focus, kLineWidth};
    chips.disabled_border.width = kLineWidth;
    if (chips.disabled_selected_border)
        chips.disabled_selected_border->width = kLineWidth;
    chips.padding = huxerui::EdgeInsets::Symmetric(10.0F, 4.0F);
    chips.minimum_height = 24.0F;
    chips.corner_radii = huxerui::CornerRadii{spec.shapes.full};
    definition.Set(chips);

    huxerui::TabsStyle tabs = huxerui::UseEnvironment<huxerui::TabsStyle>();
    tabs.background = huxerui::Color::Transparent();
    tabs.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                          spec.colors.on_surface_variant};
    tabs.selected_label = spec.colors.on_surface;
    tabs.indicator = lines.line_focus;
    tabs.indicator_height = kEmphasisLineWidth;
    tabs.indicator_corner_radius = kEmphasisLineWidth * 0.5F;
    tabs.divider_color = huxerui::Color::Transparent();
    tabs.divider_height = 0.0F;
    tabs.item_padding = huxerui::EdgeInsets::Symmetric(12.0F, 6.0F);
    tabs.minimum_height = 32.0F;
    tabs.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    tabs.indicator_animation_duration = spec.motion.normal;
    definition.Set(tabs);

    huxerui::TreeViewStyle tree = huxerui::UseEnvironment<huxerui::TreeViewStyle>();
    tree.background = huxerui::Color::Transparent();
    tree.foreground = spec.colors.on_surface;
    tree.disabled_foreground = withAlpha(spec.colors.on_surface, 0.42F);
    tree.selected_background = withAlpha(spec.colors.on_surface, 0.10F);
    tree.active_background = withAlpha(spec.colors.on_surface, 0.05F);
    tree.focus_indicator = lines.line_focus;
    tree.item_extent = 32.0F;
    tree.item_padding = 6.0F;
    tree.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.10F)},
    };
    definition.Set(tree);

    huxerui::DividerStyle divider = huxerui::UseEnvironment<huxerui::DividerStyle>();
    divider.color = lines.line_subtle;
    divider.thickness = kLineWidth;
    definition.Set(divider);

    huxerui::TextFieldStyle fields = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    fields.text_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                           spec.colors.on_surface};
    fields.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                            spec.colors.on_surface_variant};
    fields.floating_label_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip), spec.colors.on_surface_variant};
    fields.placeholder_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                                   spec.colors.on_surface_variant};
    fields.focused_label = spec.colors.primary;
    fields.caret = spec.colors.primary;
    fields.selection = withAlpha(spec.colors.primary, 0.24F);
    fields.border_width = kLineWidth;
    fields.focused_border_width = kLineWidth;
    fields.validation_border_width = kLineWidth;
    fields.focused_validation_border_width = kLineWidth;
    for (huxerui::TextFieldVariantStyle* variant : {&fields.standard, &fields.filled,
                                                    &fields.outlined}) {
        variant->background = spec.colors.surface_container_highest;
        variant->disabled_background = withAlpha(spec.colors.on_surface, 0.06F);
        variant->border = lines.line_subtle;
        variant->hovered_border = lines.line_default;
        variant->focused_border = lines.line_focus;
        variant->disabled_border = lines.line_disabled;
        variant->minimum_height = 36.0F;
        variant->corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    }
    fields.standard.background = huxerui::Color::Transparent();
    fields.filled.background = spec.colors.surface_container_low;
    fields.outlined.background = spec.colors.surface;
    fields.padding = huxerui::EdgeInsets::Symmetric(10.0F, 7.0F);
    definition.Set(fields);

    huxerui::CheckboxStyle checkboxes = huxerui::UseEnvironment<huxerui::CheckboxStyle>();
    checkboxes.unchecked_border = huxerui::Border{lines.line_default, kLineWidth};
    checkboxes.disabled_unchecked_border = huxerui::Border{lines.line_disabled, kLineWidth};
    definition.Set(checkboxes);

    huxerui::RadioButtonStyle radios = huxerui::UseEnvironment<huxerui::RadioButtonStyle>();
    radios.unselected_color = lines.line_default;
    radios.disabled_unselected_color = lines.line_disabled;
    radios.border_width = kLineWidth;
    definition.Set(radios);

    huxerui::SwitchStyle switches = huxerui::UseEnvironment<huxerui::SwitchStyle>();
    switches.unchecked_track_border = lines.line_default;
    switches.disabled_unchecked_track_border = lines.line_disabled;
    switches.track_border_width = kLineWidth;
    definition.Set(switches);

    // 内置确认框（DialogHandle::Show(title, message, ...) 形态）跟随主题：DialogStyle
    // 是 Environment 值（presentation.h 有 Default()/==），经 ThemeDefinition::Set
    // 全局覆盖。Default() 基线是白底浅色配色，逐字段换色。
    // 删除/清空等破坏性确认已统一走 ui 层 ShowDangerConfirm（确认按钮染 error 红），
    // 这里的覆盖保留为内置形态的兜底主题。
    // 叠加层（hover/press）改用 inverse_on_surface 派生的半透明色，
    // 保持深浅主题和玻璃表面的对比度。
    huxerui::DialogStyle dialogs = huxerui::UseEnvironment<huxerui::DialogStyle>();
    dialogs.background = spec.colors.surface_container_high;
    dialogs.title_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kTitle).WithWeight(huxerui::FontWeight::Bold),
        spec.colors.on_surface};
    dialogs.message_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                               spec.colors.on_surface};
    dialogs.positive_action_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                                       spec.colors.on_primary};
    dialogs.positive_action_background = spec.colors.primary;
    dialogs.positive_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{
            .fill = withAlpha(spec.colors.on_primary, 0.10F)},
        .press = huxerui::IndicationLayer{
            .fill = withAlpha(spec.colors.on_primary, 0.18F)},
    };
    dialogs.negative_action_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                                       spec.colors.on_surface};
    dialogs.negative_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    dialogs.action_separator_color = lines.line_subtle;
    dialogs.corner_radii = huxerui::CornerRadii{
        spec.shapes.large + spec.spacing.extra_small * 0.5F};
    dialogs.action_corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    definition.Set(dialogs);

    // 下拉选择（Select，全局设置页/历史页在用）跟随主题：触发框与弹出菜单
    // 使用中等圆角，和请求参数行、卡片保持同一视觉节奏。
    // 叠加层沿用 DialogStyle 的 on_surface 半透明做法，不用 M3 ripple。
    huxerui::SelectStyle selects = huxerui::UseEnvironment<huxerui::SelectStyle>();
    selects.background = spec.colors.surface_container_highest;
    selects.foreground = spec.colors.on_surface;
    selects.border = huxerui::Border{lines.line_default, kLineWidth};
    selects.indicator = spec.colors.on_surface_variant;
    selects.popup_background = spec.colors.surface_container;
    selects.active_item_background = withAlpha(spec.colors.on_surface, 0.06F);
    selects.selected_item_background = withAlpha(spec.colors.on_surface, 0.10F);
    selects.validation_error = spec.colors.error;
    selects.validation_text_style = huxerui::TextStyle{
        huxerui::Font::System(font_size::kChip), spec.colors.error};
    selects.trigger_padding = huxerui::EdgeInsets::Symmetric(spec.spacing.medium,
                                                             spec.spacing.small);
    selects.item_padding = selects.trigger_padding;
    selects.popup_shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.16F), {}, 12.0F, 0.0F};
    selects.content_spacing = spec.spacing.small;
    selects.validation_spacing = spec.spacing.extra_small;
    selects.minimum_height = 36.0F;
    selects.minimum_item_height = 36.0F;
    selects.indicator_size = 16.0F;
    selects.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    selects.popup_corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    const huxerui::Indication selectIndication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    selects.indication = selectIndication;
    selects.item_indication = selectIndication;
    definition.Set(selects);

    // 菜单类弹层统一品牌圆角。自绘的三点菜单、方法/类型选择菜单读取
    // MenuStyle；系统 Select 使用上面的 SelectStyle；可搜索环境选择读取
    // ComboBoxStyle。三条路径保持相同表面、阴影和交互反馈。
    huxerui::MenuStyle menus = huxerui::UseEnvironment<huxerui::MenuStyle>();
    menus.background = spec.colors.surface_container;
    menus.foreground = spec.colors.on_surface;
    menus.icon_tint = spec.colors.on_surface_variant;
    menus.separator_color = lines.line_subtle;
    menus.separator_thickness = kLineWidth;
    menus.shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.16F), {}, 12.0F, 0.0F};
    menus.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    menus.item_indication = selectIndication;
    definition.Set(menus);

    huxerui::ComboBoxStyle combos = huxerui::UseEnvironment<huxerui::ComboBoxStyle>();
    combos.popup_background = spec.colors.surface_container;
    combos.foreground = spec.colors.on_surface;
    combos.active_item_background = withAlpha(spec.colors.on_surface, 0.06F);
    combos.item_padding = selects.item_padding;
    combos.popup_shadow = selects.popup_shadow;
    combos.minimum_item_height = selects.minimum_item_height;
    combos.maximum_popup_height = selects.maximum_popup_height;
    combos.popup_corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    combos.item_indication = selectIndication;
    definition.Set(combos);

    return huxerui::Theme(definition, content);
}

huxerui::View MinimalThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    return huxerui::MaterialTheme(spec, MinimalThemeStyles(content));
}

// 标题栏标签的几何、交互与拖拽实现见 title_bar.cpp；本文件负责顶级状态与页面保活。

// 左列：图标侧边栏（选中态用实心图标变体 + 药丸底色，悬停显示文字提示）。
// 侧栏不做岛、直接落在海面（窗口背景）上；选中强调只有图标变体与药丸底色——
// 原先的左侧 3pt 指示条既是多余的第三条选中线索，又会占掉行内水平空间、把图标
// 从药丸中心推开，已删除。导航按钮为侧栏专用档：44pt 命中区（compact 36）+
// 22pt 图标（compact 20）——官方 IconButton 固定小图标且命中区窄，与选中药丸
// 范围对不上；这里 hover/press 反馈与选中底色画在同一块 Frame 上（手工
// Indication，与 AppIconButton 同配方），范围严格一致。
// WebSocket/TCP 已并入请求页标签，不再单列。
inline constexpr float kCompactSideShellWidth = 52.0F;
inline constexpr float kRegularSideShellWidth = 64.0F;

huxerui::View SideRailButton(const huxerui::ThemeSpec& theme, huxerui::ImageResource icon,
                             const char* tooltip, bool selected, bool compact,
                             std::function<void()> onClick) {
    const float target = compact ? 36.0F : 44.0F;
    const float iconSize = compact ? 20.0F : 22.0F;
    // hover/press 反馈 = on_surface 6%/12% 叠层（深浅主题自适应），与
    // AppIconButton 同一配方；范围 = 整块命中区，与选中药丸底完全一致
    //（此前官方 IconButton 的默认悬停只有其自身窄命中区，与药丸错位）。
    huxerui::Color hoverTint = theme.colors.on_surface;
    hoverTint.alpha = 0.06F;
    huxerui::Color pressTint = theme.colors.on_surface;
    pressTint.alpha = 0.12F;
    huxerui::Indication hitIndication;
    hitIndication.hover = huxerui::IndicationLayer{
        .fill = huxerui::VisualFill{huxerui::Brush{hoverTint}}};
    hitIndication.press = huxerui::IndicationLayer{
        .fill = huxerui::VisualFill{huxerui::Brush{pressTint}}};
    return huxerui::Row{
               // 图标必须 Tint 主题色：SVG 资源按原色渲染，深色海面上会看不清
               //（此前官方 IconButton 内部自带主题 Tint，换裸 Image 时丢了）。
               // 非选中 = on_surface_variant（导航惯例次级色），选中 =
               // on_primary_container（与 primary_container 药丸底配对）。
               huxerui::Image(icon)
                   .Fit(huxerui::ImageFit::Contain)
                   .Align(huxerui::HorizontalAlignment::Center,
                          huxerui::VerticalAlignment::Center)
                   .Tint(selected ? theme.colors.on_primary_container
                                  : theme.colors.on_surface_variant)
                   .With(huxerui::Frame{.width = iconSize, .height = iconSize}),
           }
        .With(huxerui::Frame{.width = target, .height = target},
              huxerui::Background(selected ? theme.colors.primary_container
                                           : huxerui::Color::Transparent()),
              huxerui::CornerRadius(theme.shapes.small),
              std::move(hitIndication),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = tooltip},
              huxerui::Tooltip(tooltip),
              huxerui::Focusable(true))
        .OnClick(std::move(onClick));
}

[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
        std::size_t page; // 目标页码（PageIndex），不再用下标推算
    };
    const std::array<Item, 4> items{
        Item{app::images::request, app::images::request_selected, "请求", pages::kRequest},
        Item{app::images::loadtest, app::images::loadtest_selected, "压测", pages::kLoad},
        Item{app::images::history, app::images::history_selected, "历史记录", pages::kHistory},
        Item{app::images::gear, app::images::gear_selected, "项目设置", pages::kProjectSettings},
    };

    std::vector<huxerui::View> buttons;
    for (const Item& item : items) {
        const std::size_t page = item.page;
        const bool selected = navPage.Get() == page;
        const huxerui::ImageResource& icon = selected ? item.icon_selected : item.icon;
        buttons.push_back(SideRailButton(theme, icon, item.tooltip, selected, compact,
                                         [navPage, page] { navPage = page; }));
    }
    return huxerui::Column(std::move(buttons))
        .With(huxerui::Padding(theme.spacing.small),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = compact ? kCompactSideShellWidth : kRegularSideShellWidth},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 底部状态条已移至 global_status_bar.cpp（P1-C2 纯搬移）

} // namespace

// 应用安装钩子（AppOptions::application_hooks，运行时在工作排队前调用一次）：
// 提供托盘窗口控制器 + 注册托盘激活处理器。
//
// 这里是托盘激活的**唯一**注册点，原因见 ui/app.h：OnActivate 注册的是"应用级
// 主激活处理器"，活到 Runtime 关闭，重复注册抛 std::logic_error。旧版 HuxerUI
// 的 OnActivate(handler, deps...) 自己把它包进 Lifecycle（组合期订阅 + 依赖
// 重连），所以在 AppRoot 里每次重组调用都安全；08acc36 所有权重构把它改成应用
// 级一次性注册后，原来的写法在第二次重组时抛异常 → std::terminate（启动后
// 托盘宿主就绪触发重组即闪退）。处理器不捕获窗口，改为捕获控制器。
namespace {
// 进程级窗口控制器指针（应用安装期创建，进程内唯一）：控制面命令要用它操作窗口，
// 而 UseService 只在组合期可用。
TrayWindowController* g_windowController = nullptr;
} // namespace

void PublishWindowController(TrayWindowController* controller) { g_windowController = controller; }

TrayWindowController* CurrentWindowController() { return g_windowController; }

void InstallSystemTray(huxerui::ApplicationContext& context) {
    auto controller = std::make_shared<TrayWindowController>();
    PublishWindowController(controller.get());
    context.Provide(controller);
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    // ApplicationHandle 供轻量模式退出时重建主窗口（ShowWindow 的 OpenWindow 路径）。
    controller->SetApplication(application);
    application.SystemTray().OnActivate([controller] { controller->ShowWindow(); });
}

void TrayWindowController::ShowWindow() {
    if (window_ && !retired_) {
        window_->Show();
        window_->Activate();
        return;
    }
    if (!application_) return;
    // 重建主窗口：与 src/app.cpp 的 AppOptions.window 同值（title/尺寸/chrome），
    // 两处需同步修改。新窗口显式使用 AppRoot 内容工厂，会话持久化
    // 的标签/主题/关闭行为按启动路径恢复；未持久化的组合态不继承。
    huxerui::WindowOptions options;
    options.title = "apitab — API 测试与压测";
    options.initial_size = {1180.0F, 760.0F};
    options.minimum_size = huxerui::Size{800.0F, 600.0F};
    options.chrome_mode = huxerui::WindowChromeMode::Custom;
    options.title_bar_height = 24.0F;
    const huxerui::WindowHandle rebuilt = application_->OpenWindow([] { return AppRoot(); }, options);
    retired_ = false;
    rebuilt.Show();
    rebuilt.Activate();
}

// 关闭询问弹窗宿主：必须在 MinimalThemed provider 之下组合——AppRoot 自身在
// provider 之上，层内容捕获调用处环境，在 AppRoot 里 dialog.Show 的弹窗
// UseTheme() 只能拿到默认浅色 spec（弹窗不应用主题的根因）。关闭拦截
// （OnCloseRequest）与询问弹窗都挂在这里；content 原样返回，仅附加行为。
// CloseGuard 已移至 app_dialogs.cpp（P1-C2 纯搬移）

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    // 托盘激活处理器已由 InstallSystemTray（应用安装钩子）一次性注册；这里只取
    // 它用来定位窗口的应用级控制器。必须无条件取服务，不能放进 if (trayAvailable)。
    const auto trayWindow = huxerui::UseService<TrayWindowController>();
    // 多窗口控制器（InstallWindowManager 应用级 Provide）：主窗口桥 + 拆出窗口创建。
    const auto windowManager = huxerui::UseService<WindowManager>();
    auto toast = huxerui::UseToast();
    // IsAvailable() 内部会观察托盘可用性 State：DBus 托盘宿主就绪较晚时，
    // 这里在组合期订阅，可用性翻转后本作用域重组、托盘随后注册。
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();
    // 控制面（`apitab --cli` 的服务端）在应用安装期起来，那时还没有组合作用域；
    // 这里把根作用域的 TaskScope::Post 注入它的投递口，命令才能回到应用线程执行。
    const auto controlPoster = huxerui::UseService<apitab::control::ApplicationPoster>();
    huxerui::Lifecycle(
        [controlPoster, tasks] {
            controlPoster->Set([tasks](std::function<void()> task) { tasks.Post(std::move(task)); });
            return [controlPoster] { controlPoster->Clear(); };
        },
        0);
    auto loggedIn = huxerui::UseState(true);
    auto loginDialog = huxerui::UseDialog();
    // UseState 的 initial 参数在重组时仍会先求值；用一个一次性门闩避免把启动恢复
    // 与磁盘读取误放进每次 AppRoot 重组。头像稍后由生命周期任务异步加载。
    auto initialized = huxerui::UseState(false);
    const bool firstComposition = !initialized.Get();
    auto avatarImage = huxerui::UseState(huxerui::ImageAsset{});

    huxerui::Lifecycle([tasks, avatarImage] {
        const std::filesystem::path avatarPath = cfg::dataDir() / "avatar.png";
        tasks.Launch([avatarPath, avatarImage]() -> huxerui::Task<void> {
            try {
                const huxerui::ImageAsset image = co_await huxerui::RunWorker([avatarPath] {
                    std::error_code ec;
                    if (!std::filesystem::is_regular_file(avatarPath, ec) || ec)
                        return huxerui::ImageAsset{};
                    return huxerui::ImageAsset::FromFile(avatarPath);
                });
                if (image.HasValue()) avatarImage = image;
            } catch (const std::exception&) {
                // 头像是可选装饰资源，读取失败时保留默认头像，不打断应用启动。
            }
        });
    });

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色，未保存偏好时默认跟随系统。
    int initialThemeMode = 0;
    if (firstComposition && sessionPreference("theme_mode") == "1") initialThemeMode = 1;
    if (firstComposition && sessionPreference("theme_mode") == "2") initialThemeMode = 2;
    // 关闭行为：0=每次询问 1=直接关闭 2=最小化到托盘
    int initialCloseBehavior = 0;
    if (firstComposition && sessionPreference("close_behavior") == "1") initialCloseBehavior = 1;
    if (firstComposition && sessionPreference("close_behavior") == "2") initialCloseBehavior = 2;

    // ---- 顶级标签状态（island-structure-theme.md §13.1/§13.2，P1-B0.1）----
    // navPage：项目工作区内部页（kRequest/kLoad/kHistory/kProjectSettings），不再包含
    // kHome/kAppSettings 等顶级目的地。
    // tabs：打开的项目标签顺序（= 持久化 open_projects 的 CSV 格式）。
    // activeProject：领域当前项目游标——不再兼任「当前顶级标签」；设置激活时
    //   不清空（避免返回项目后重新加载，§13.1），视觉 active 由 activeTopTab 决定。
    // settingsOpen：设置单例标签是否存在（不持久化，启动 = 无设置标签）。
    // activeTopTab：当前顶级标签（Home / Project(id) / GlobalSettings；不持久化，
    //   启动 = 主页）。
    // lastProjectTab：最近激活且仍打开的项目 id（关设置回退用；0 = 无，不持久化）。
    auto navPage = huxerui::UseState<std::size_t>(pages::kRequest);
    // P1-B0.5 启动恢复：从 session.open_projects / session.active_project 重建 tabs 与 active（解析/去重/过滤已删，数据无效回主页）。
    TopTabState restored;
    if (firstComposition) {
        const auto projects = g_requests.allProjects();
        if (projects) {
            std::vector<std::int64_t> existIds;
            for (const db::Project& p : *projects) existIds.push_back(p.id);
            restored = RestoreTopTabs(sessionPreference("open_projects"),
                                      sessionPreference("active_project"), existIds);
        }
    }
    // 领域游标与 State 同步（启动时即一致，首帧不闪回主页）。
    if (firstComposition) {
        if (restored.active.kind == TopTabKind::Project) {
            if (auto selected = g_requests.selectProject(restored.active.project_id); !selected) {
                toast.Show("恢复项目失败: " + selected.error().message);
                restored = {};
            }
            if (auto loaded = g_loadtest.setProject(restored.active.project_id); !loaded)
                toast.Show("加载压测配置失败: " + loaded.error().message);
        } else {
            if (auto selected = g_requests.selectProject(0); !selected)
                toast.Show("恢复主页项目上下文失败: " + selected.error().message);
            if (auto loaded = g_loadtest.setProject(0); !loaded)
                toast.Show("加载压测配置失败: " + loaded.error().message);
        }
    }
    auto tabs = huxerui::UseState(firstComposition ? std::move(restored.open_projects)
                                                   : std::vector<std::int64_t>{});
    auto activeProject = huxerui::UseState(
        firstComposition && restored.active.kind == TopTabKind::Project
            ? restored.active.project_id
            : std::int64_t{0});
    auto settingsOpen = huxerui::UseState(firstComposition && restored.settings_open);
    auto activeTopTab = huxerui::UseState(firstComposition ? restored.active : TopTabId{});
    auto lastProjectTab = huxerui::UseState(firstComposition ? restored.last_project : std::int64_t{0});
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto closeBehavior = huxerui::UseState<int>(std::move(initialCloseBehavior));
    auto closeDialogOpen = huxerui::UseState(false);
    // P1-B0.5 状态保活：设置分类在 AppRoot，随顶级标签存活（切到项目再回保留原分类）
    auto settingsCategory = huxerui::UseState<std::size_t>(0);

    // 生命周期 setup 在首帧提交后运行；从此以后 AppRoot 重组只消费已挂载 State，
    // 不再重复执行启动恢复的数据库查询与领域重载。
    huxerui::Lifecycle([initialized] { initialized = true; });

    // ---- 顶级标签操作 ----
    // 变更本体（领域写入 + State 写回）在事件回调内同步完成：切到 Project(id) 时
    // 先做领域写入再写 activeTopTab，同一回调内两者一致（§13.2 不变量 1）。

    // TopTabState 快照（与上面的 State 一一对应，见 ui.h TopTabState）。
    auto topTabSnapshot = [=]() -> TopTabState {
        TopTabState s;
        s.open_projects = tabs.Get();
        s.settings_open = settingsOpen.Get();
        s.active = activeTopTab.Get();
        s.last_project = lastProjectTab.Get();
        return s;
    };

    // 领域同步：active 变为 Project(id) 时无条件写领域（§13.2 项 3：不能因
    // project id 未变化而跳过顶级切换；selectProject/setProject 幂等）。
    // activeProject State 与 store 游标同步（HomePage is_open 高亮 / RequestPage
    // 的领域输入）。
    auto syncDomainProject = [=](std::int64_t id) {
        if (auto selected = g_requests.selectProject(id); !selected) {
            toast.Show("切换项目失败: " + selected.error().message);
            return false;
        }
        if (auto loaded = g_loadtest.setProject(id); !loaded)
            toast.Show("加载压测配置失败: " + loaded.error().message);
        saveSessionPreference("active_project", std::to_string(id));
        activeProject = id;
        return true;
    };

    // State 写回；open_projects 变化时按既有 CSV 格式持久化（保持「只在新增/拖拽
    // 时持久化」的惯例——激活已打开项目不重写）。settingsOpen/activeTopTab/
    // lastProjectTab 不持久化（启动 = 主页、无设置标签，§13.4 B0.1）。
    auto commitTopTab = [=](const TopTabState& before, const TopTabState& after) {
        if (after.open_projects != before.open_projects) {
            std::string csv;
            for (std::size_t i = 0; i < after.open_projects.size(); ++i) {
                csv += (i ? "," : "");
                csv += std::to_string(after.open_projects[i]);
            }
            saveSessionPreference("open_projects", csv);
        }
        tabs = after.open_projects;
        settingsOpen = after.settings_open;
        activeTopTab = after.active;
        lastProjectTab = after.last_project;
    };

    // 激活顶级标签（主页/项目/设置统一入口；点击项目标签无条件激活顶级 + 领域，
    // 即使 activeProject 已是该 id，§13.2 项 3）。
    auto activateTopTabNow = [=](TopTabId target) {
        const TopTabState before = topTabSnapshot();
        TopTabState after = before;
        switch (target.kind) {
            case TopTabKind::Home:
                after = ActivateHome(before);
                break;
            case TopTabKind::Project:
                after = ActivateProject(before, target.project_id);
                break;
            case TopTabKind::GlobalSettings:
                after = ActivateSettings(before);
                break;
        }
        if (after.active.kind == TopTabKind::Project) {
            if (!syncDomainProject(after.active.project_id)) return;
        }
        commitTopTab(before, after);
    };

    // 关闭顶级标签。领域同步规则：回退到项目则同步领域；活动项目被关且回主页则
    // 领域清零（旧模型行为，状态条随之显示「未打开项目」）；关设置回主页不清领域
    //（项目仍在打开列表，保留游标避免重开时重新加载）。
    auto closeTopTabNow = [=](TopTabId target) {
        const TopTabState before = topTabSnapshot();
        const TopTabState after = CloseTopTab(before, target);
        if (after.active.kind == TopTabKind::Project) {
            if (!syncDomainProject(after.active.project_id)) return;
        } else if (target.kind == TopTabKind::Project && before.active == target) {
            if (auto selected = g_requests.selectProject(0); !selected) {
                toast.Show("切回主页失败: " + selected.error().message);
                return;
            }
            if (auto loaded = g_loadtest.setProject(0); !loaded)
                toast.Show("加载压测配置失败: " + loaded.error().message);
            saveSessionPreference("active_project", "0");
            activeProject = 0;
        }
        commitTopTab(before, after);
    };

    auto activateTopTab = activateTopTabNow;
    auto closeTopTab = closeTopTabNow;

    // ---- 多窗口：主窗口桥 + 焦点驱动的领域游标重同步 ----
    // detach_tab：拆出/子窗口关闭时移除主窗口侧标签——状态回退与持久化同关闭
    // 标签，但不写领域游标（游标归发起方：拆出后归子窗口、关闭后随主窗口焦点
    // 重同步归位）。
    auto detachTopTab = [=](std::int64_t id) {
        const TopTabState before = topTabSnapshot();
        const TopTabState after = CloseTopTab(before, TopTabId{TopTabKind::Project, id});
        commitTopTab(before, after);
    };
    huxerui::Lifecycle(
        [windowManager, window, detachTopTab, activateTopTabNow, navPage] {
            windowManager->SetMainWindow(window);
            windowManager->SetMainBridge(MainBridge{
                .detach_tab = detachTopTab,
                .restore_tab =
                    [activateTopTabNow](std::int64_t id) {
                        activateTopTabNow(TopTabId{TopTabKind::Project, id});
                    },
                .wake_to_tab = activateTopTabNow,
                .current_nav_page = [navPage] { return navPage.Get(); },
            });
            return [windowManager] {
                windowManager->ClearMainBridge();
                windowManager->ClearMainWindow();
            };
        },
        0);

    // 焦点驱动重同步：本窗口回到前台时把领域游标拉回自己的活动顶级标签，
    // 防止另一窗口操作期间游标漂移导致本窗口读到别的项目数据。游标已一致时
    // 不写（避免多余重组）；不一致时写 activeProject 触发本窗口重组刷新。
    window.OnLifecycleChanged(
        [activeTopTab, activeProject, toast](const huxerui::WindowLifecycleState state) {
            if (state != huxerui::WindowLifecycleState::Active) return;
            const TopTabId active = activeTopTab.Get();
            const std::int64_t want = active.kind == TopTabKind::Project ? active.project_id : 0;
            if (g_requests.currentProjectId() == want) return;
            if (auto selected = g_requests.selectProject(want); !selected) {
                toast.Show("切换项目失败: " + selected.error().message);
                return;
            }
            if (auto loaded = g_loadtest.setProject(want); !loaded)
                toast.Show("加载压测配置失败: " + loaded.error().message);
            saveSessionPreference("active_project", std::to_string(want));
            activeProject = want;
        });

    TopTabActions topTabActions{
        activateTopTab,
        closeTopTab,
        // 项目标签拖出标签条：拆成独立子窗口（Chrome 式 tear-off）。落点屏幕
        // 坐标 = 本窗口客户区原点 + 拖拽指针窗口内位置；Position 在 Wayland 无
        // 值，子窗口退化为窗口管理器默认摆放。
        [windowManager, window](
            std::int64_t id, const huxerui::Point pointer) {
            std::optional<huxerui::Point> screen;
            if (const std::optional<huxerui::Point> origin = window.Position();
                origin.has_value()) {
                screen = huxerui::Point{origin->x + pointer.x, origin->y + pointer.y};
            }
            windowManager->TearOff(kMainWindowId, id, screen);
        },
    };

    // HomePage 打开项目回调：ProjectCard 在完成领域写入后调用，内部走同一
    // activateTopTabNow——新增/激活顶级项目标签 + State
    // 写回 + open_projects 按需持久化（格式与原 CSV 一致）。
    std::function<void(std::int64_t)> onOpenProject = [activateTopTabNow](std::int64_t id) {
        activateTopTabNow(TopTabId{TopTabKind::Project, id});
    };

    // HomePage 删除项目回调（卡片菜单「删除」确认后）：先关掉该项目
    // 已打开的顶级标签——CloseTopTab 负责 active/last_project 回退与领域清零，避免
    // 留下指向已删项目的空标签——再从库里删（级联删其分组/请求/环境）。返回空串 =
    // 成功，非空 = 错误消息（卡片 toast）。
    std::function<std::string(std::int64_t)> onDeleteProject =
        [closeTopTabNow](std::int64_t id) -> std::string {
        closeTopTabNow(TopTabId{TopTabKind::Project, id});
        const Status removed = g_requests.deleteProject(id);
        return removed ? std::string{} : removed.error().message;
    };

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    // 注意：AppRoot 里的 UseTheme() 拿到的是 MaterialTheme provider 之上（应用外）
    // 的默认浅色 spec——主题由本函数返回时包进子树，自身读不到。所以根节点自身的
    // 配色（整窗背景、标题栏底、间距）必须直接按 dark 选 spec；子组件在 provider
    // 之下，它们的 UseTheme() 是正常的。
    const huxerui::ThemeSpec rootSpec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    // Compact（<900dp）收窄侧栏与标题栏水平留白，覆盖 800dp 最小桌面窗口。
    // 根 Column 不设 Spacing，页面外边距由 PageCard 的 8dp Padding 提供。
    // gap 仅用于标题栏内部间距；项目状态栏由各项目 PageCard 在内部承载。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const float gap = rootSpec.spacing.extra_small;
    const float titleBarHorizontalPadding =
        compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small;
    const float sideShellWidth =
        compact ? kCompactSideShellWidth : kRegularSideShellWidth;
    const float titleBarLogoWidth = sideShellWidth - 2.0F * titleBarHorizontalPadding;

    // 托盘：把当前窗口交给应用级控制器——托盘激活处理器（InstallSystemTray
    // 注册）经它激活主窗口。句柄在根挂载时写入、卸载时清除。
    huxerui::Lifecycle(
        [trayWindow, window] {
            trayWindow->SetWindow(window);
            return [trayWindow] { trayWindow->ClearWindow(); };
        },
        0);

    // 托盘图标 + 菜单；仅在可用时展示：首次组合时宿主未就绪则跳过，
    // 待可用性翻转触发重组后再 Show。
    if (trayAvailable) {
        huxerui::Lifecycle(
            [tray, trayWindow, application, tasks] {
                // 菜单项用 push_back 构造：GCC 16 对 menu 列表初始化内的
                // MenuItem/MenuSection 隐式转换报 "expected primary-expression"
                std::vector<huxerui::MenuEntry> menuEntries;
                // 走控制器而非捕获窗口句柄：轻量模式期间句柄已退役，
                // ShowWindow 会按需重建主窗口。
                menuEntries.push_back(
                    huxerui::MenuItem("显示主窗口", [trayWindow] { trayWindow->ShowWindow(); }));
                // 轻量模式（伪纯 CLI 形态）：销毁全部窗口（fork 级联退役）真正
                // 释放渲染内存，控制面继续服务；托盘/`show` 命令可唤回重建。
                // Enter 在系统托盘回调返回后执行，避免回调栈内销毁窗口。
                menuEntries.push_back(huxerui::MenuItem(
                    "轻量模式（销毁窗口，释放内存）", [tasks] {
                        tasks.Launch([]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            (void)EnterLightweightMode();
                        });
                    }));
                menuEntries.push_back(huxerui::MenuSection{});
                menuEntries.push_back(
                    huxerui::MenuItem("退出", [application] { application.Quit(); }));
                // SystemTray 当前只接受可解析为 ImageAsset 的栅格资源；tray.png/@2x/@3x
                // 是水母徽标的光栅版本（由 resources/images/apitab_tray.svg 生成），
                // 不能直接传 SVG 的 VectorAsset。
                tray.Show(app::images::tray,
                          huxerui::SystemTrayOptions{
                              .tooltip = "apitab — API 测试与压测",
                              .menu = std::move(menuEntries)});
                return [tray] { tray.Hide(); };
            },
            0);
    }

    // 内容区（P1-B0.1）：按 activeTopTab 切换顶级标签内容。主页 / 全局设置整宽
    // 覆盖侧栏（都与项目无关，未打开项目就点不到任何项目相关入口）；Project(id)
    // → 侧栏 + PageFor（项目工作区内部页，navPage 表达）。防御：active 项目 id 不在
    // tabs（open_projects）时回落主页——该路径不应发生（activeTopTab 只经
    // ActivateProject/CloseTopTab 变更，不变量 1 保证 active 项目在 open_projects 中）。
    const TopTabId activeTab = activeTopTab.Get();
    bool showSideShell = false;
    // P1-B0.5 状态保活：顶级标签内容用 IndexedPages 保持所有页面挂载（设置↔项目切换不卸载，草稿保活）
    std::vector<huxerui::View> indexed;
    indexed.reserve(1 + tabs.Get().size() + (settingsOpen.Get() ? 1 : 0));
    indexed.push_back(pages::PageCard(
                          HomePage(onOpenProject, onDeleteProject, activeProject))
                          .With(huxerui::Grow(1.0F)));
    std::unordered_map<std::int64_t, std::size_t> projIdx;
    for (std::int64_t id : tabs.Get()) {
        const std::size_t idx = indexed.size();
        projIdx[id] = idx;
        indexed.push_back(pages::PageCard(pages::ProjectPages(navPage, activeProject, kMainWindowId), true)
                              .Key(id)
                              .With(huxerui::Grow(1.0F)));
    }
    if (settingsOpen.Get()) {
        indexed.push_back(pages::PageCard(
                               GlobalSettingsPage(themeMode, closeBehavior, settingsCategory,
                                                  avatarImage))
                               .Key(kSettingsTabDisplayKey)
                               .With(huxerui::Grow(1.0F)));
    }
    std::size_t selected = 0;
    if (activeTab.kind == TopTabKind::Project) {
        const auto it = projIdx.find(activeTab.project_id);
        if (it != projIdx.end()) {
            selected = it->second;
            showSideShell = true;
        }
    } else if (activeTab.kind == TopTabKind::GlobalSettings) {
        selected = indexed.size() > 0 ? indexed.size() - 1 : 0;
    }
    if (selected >= indexed.size()) selected = 0;
    huxerui::View page = huxerui::IndexedPages(std::move(indexed), selected);
    // IndexedPages keep-alive（P1-B0.5）：切主题只是根重组、IndexedPages 保持所有页面挂载
    // （设置↔项目切换不卸载，草稿与设置分类保活）；切项目/切内部页仅切换 selected 索引。

    auto topAvatarHovered = huxerui::UseState(false);
    huxerui::View topAvatar = ProfileAvatar(avatarImage.Get(), 28.0F, rootSpec, topAvatarHovered.Get()).With(
        huxerui::Tooltip("用户登录"),
        huxerui::Focusable(true),
        huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = "用户登录"});
    topAvatar = std::move(topAvatar).On<huxerui::ViewEvents::Hover>(
        [topAvatarHovered](const huxerui::HoverEvent& event) {
            topAvatarHovered = event.type != huxerui::HoverEventType::Leave;
        }).OnClick([dark, loginDialog, loggedIn] {
        loginDialog.Show(
            [dark, loggedIn](huxerui::DialogContext ctx) -> huxerui::View {
                return MinimalThemed(dark, LoginPage(ctx, loggedIn));
            },
            huxerui::DialogOptions{});
    });

    huxerui::View content = huxerui::Column {
        // 自定义标题栏使用窗口底色：Logo + 顶级标签条 + 齿轮 + 头像，框架在右侧渲染窗口按钮。
        // 标签和操作内容的基准高度为 24dp；头像为 28dp，系统按钮也可能提高最终标题栏高度。
        // WindowTitleBar 构造即带交叉轴居中，这里给中间标签条包装 Row 也补上
        // 居中，任何一侧偏高都不漂移。
        huxerui::WindowTitleBar {
            TitleBarLogo(titleBarLogoWidth).With(huxerui::WindowDragRegion{}),
            // 标签条按真实内容宽度布局；窄窗时受父约束收缩并横向滚动。
            TopTabStrip(tabs, settingsOpen, activeTopTab, topTabActions, false),
            // 标题栏唯一的弹性项。ScrollView 本身必须保持 Client；所有标签内容之外
            // 的剩余宽度由这个真实 sibling 占有，因此整块空白可靠命中 Drag，右侧
            // 动作组仍按自然宽度固定在系统按钮左侧。
            huxerui::Spacer{}.With(huxerui::Grow(1.0F), huxerui::WindowDragRegion{}),
            // 齿轮不用 IconButton（框架内置最小触摸尺寸，Frame 压不住、比标签高
            // 一截）：裸 Image + 自绘热区。无底无圆角（设计要求去背景），
            // 与标题栏内容等高 + 交叉轴居中保证垂直居中。
            // 着色：gear.svg 是 fill="#000000" 的矢量资源，Foreground 修饰符
            // 对 Image 不起 tint 作用（用户复测深/浅色下都仍是黑色）；SDK 公开
            // API Image::Tint(Color)（view.h，内部走 DrawImage(VectorAsset,…,tint)
            // 通道）才是矢量图的着色入口——改用 .Tint(on_surface)，深底白/浅底黑。
            // 本作用域在主题 provider 之上，颜色取 rootSpec 而非 UseTheme()。
            // 偏上的根因：gear.svg 画布 24x24，大于 14pt 的 Frame——未指定
            // Fit 时按画布原始尺寸绘制并从边缘锚定。显式 Fit(Contain) +
            // Align(Center, Center) 让图案缩放后钉在框中心。
            // 齿轮：打开/激活设置单例标签（未开则开、已开仅激活，§13.1）；顶级
            // 状态写回见上。
            huxerui::Row {
                huxerui::Image(app::images::gear)
                    .Fit(huxerui::ImageFit::Contain)
                    .Align(huxerui::HorizontalAlignment::Center,
                           huxerui::VerticalAlignment::Center)
                    .Tint(rootSpec.colors.on_surface)
                    .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                      huxerui::Frame{.height = kTitleBarContentHeight},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Tooltip("全局设置"),
                      // 键盘/语义（P1-B0.4）：裸图标按钮支持键盘激活设置
                      // 单例标签（§13.6 键盘要求）。
                      huxerui::Focusable(true),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = "全局设置"})
                .OnClick([activateTopTab] {
                    activateTopTab(TopTabId{TopTabKind::GlobalSettings, 0});
                }),
            // 用户头像：固定圆形命中区；点击打开登录弹窗。
            topAvatar,
        }
            // 壳层导航直接使用窗口背景；垂直零内边距，页面与标题栏的留白由 PageCard 提供。
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      titleBarHorizontalPadding, 0.0F)),
                  huxerui::Spacing(gap)),
        // 主行：图标侧栏 + 页面卡片（项目状态栏位于卡片内）；Grow 取得标题栏之外的剩余高度。
        // 仅 Project(id) 顶级标签显示侧栏；主页/设置整宽覆盖。
        // 占位必须用空 Row——Spacer 自带 Grow(1)，会分走一半宽度。
        huxerui::Row {
            showSideShell ? huxerui::View{SideShell(navPage)}
                          : huxerui::View{huxerui::Row{}},
            // IndexedPages 的子页面有 Grow，但外层容器本身也必须作为主 Row 的
            // 弹性项接收“侧栏之外的剩余宽度”。漏掉这里会按页面固有宽度测量，
            // 请求编辑器右岛向窗口外溢出并被裁切。
            std::move(page).With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
    }
                               .With(
                                     // 根列分区紧贴，卡片统一承载页面、状态栏和内部结构线。
                                     // 窗口整体背景由主题背景色刷满。
                                     huxerui::Background(rootSpec.colors.background),
                                     // 交叉轴必须 Stretch：否则主行（侧栏+页面区）按内容
                                     // 收缩成内容宽，页面里所有 Grow 失去上界、岛屿无法
                                     // 占满逻辑区块（首页整体漂移/右对齐的根因）。
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 主题边界走 MinimalThemed：自定义品牌 spec + 组件 typed style 覆盖。
    // 关闭询问弹窗宿主挂在 provider 之下（AppRoot 自身读不到主题，CloseGuard 能）。
    return MinimalThemed(dark, CloseGuard(closeBehavior, closeDialogOpen, content));

}

// 子窗口根（拆出的项目工作区窗口）：外观与主窗口完全一致（Custom 标题栏 24pt、
// 标签条、侧栏、状态条、同一主题），唯一差别是身份——主窗口全局唯一，这里只有
// 一个项目标签：
//   - 主页标签点击 = 单纯唤醒主窗口并切主页（WakeMainToHome），自己不导航；
//   - 齿轮 = 唤醒主窗口打开全局设置；头像 = 唤醒主窗口（账号页在主窗口）；
//   - 项目标签拖出窗口顶缘 = 拖回主窗口（MergeBack，草稿随信封迁移）；
//   - ✕（标题栏或标签）= 直接关闭该标签（主窗口 open_projects 移除并持久化），
//     然后退役本窗口；
//   - 焦点回到本窗口 = 领域游标重同步到本窗口项目（单游标 store 的多窗口治理）。
// 主窗口专属接线（托盘/控制面/会话恢复/关闭询问弹窗）不在这里执行；拆窗时由
// ApplicationHandle::OpenWindow 显式传入此子窗口内容工厂。
[[huxerui::composable]] huxerui::View ChildWindowRoot(std::int64_t projectId) {
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const auto windowManager = huxerui::UseService<WindowManager>();
    auto toast = huxerui::UseToast();

    // 组合态：单项目工作区。初值仅在首组合消费一次——navPage/草稿从转移信封
    // 恢复（拆出瞬间主窗口的内部页与未保存请求），信封不在或已消费则取缺省
    //（重组时 UseState 丢弃初值，重复调用 Consume* 返回空、无害）。
    const std::optional<std::size_t> restoredNav = windowManager->ConsumeNavPage(projectId);
    auto navPage = huxerui::UseState<std::size_t>(restoredNav.value_or(std::size_t{pages::kRequest}));
    auto activeProject = huxerui::UseState(projectId);
    auto activeTopTab = huxerui::UseState(TopTabId{TopTabKind::Project, projectId});
    auto tabs = huxerui::UseState<std::vector<std::int64_t>>(std::vector<std::int64_t>{projectId});
    auto settingsOpen = huxerui::UseState(false);
    auto themeMode = huxerui::UseState([] {
        // 子窗口没有设置页：主题跟随会话偏好（主窗口修改主题对已开子窗口不
        // 追溯，重启/新开子窗口生效）。
        if (sessionPreference("theme_mode") == "1") return 1;
        if (sessionPreference("theme_mode") == "2") return 2;
        return 0;
    }());

    // 挂载登记 + 卸载清理（退役/应用退出都会走清理，窗口表不悬挂）。
    // navPage 提供者给实时拖回的信封带走子窗口的工作区内部页。
    huxerui::Lifecycle(
        [windowManager, window, projectId, navPage] {
            windowManager->RegisterChild(projectId, window, projectId);
            windowManager->RegisterNavPage(projectId, [navPage] { return navPage.Get(); });
            return [windowManager, projectId] {
                windowManager->UnregisterChild(projectId);
                windowManager->UnregisterNavPage(projectId);
            };
        },
        0);

    // 标题栏 ✕ = 直接关闭该标签（等价主窗口内 ✕），随后退役本窗口。
    window.OnCloseRequest([windowManager, projectId] {
        windowManager->CloseChildTab(projectId);
        return true;
    });

    // 焦点驱动重同步（与主窗口同一治理规则）。
    window.OnLifecycleChanged(
        [projectId, activeProject, toast](const huxerui::WindowLifecycleState state) {
            if (state != huxerui::WindowLifecycleState::Active) return;
            if (g_requests.currentProjectId() == projectId) return;
            if (auto selected = g_requests.selectProject(projectId); !selected) {
                toast.Show("切换项目失败: " + selected.error().message);
                return;
            }
            if (auto loaded = g_loadtest.setProject(projectId); !loaded)
                toast.Show("加载压测配置失败: " + loaded.error().message);
            activeProject = projectId;
        });

    // 领域兜底：拆出流程已在 OpenWindow 前同步游标；信封缺失的异常路径（如
    // 手动重建窗口）在首组合时再同步一次。首次组合后游标一致，重复执行无效果。
    if (g_requests.currentProjectId() != projectId) {
        if (auto selected = g_requests.selectProject(projectId); selected) {
            (void)g_loadtest.setProject(projectId);
            activeProject = projectId;
        }
    }

    // 顶级标签操作：主页/设置路由到主窗口（身份是子窗口），项目标签就地激活，
    // ✕ 关标签并关窗口，拖出标签条 = 拖回主窗口。
    TopTabActions topTabActions{
        [windowManager, projectId](const TopTabId target) {
            switch (target.kind) {
                case TopTabKind::Home:
                    windowManager->WakeMainToHome();
                    break;
                case TopTabKind::Project:
                    // 本窗口只有这一个项目标签：激活 = 确保游标在本项目
                    //（正常情况下焦点重同步已覆盖；点击是显式兜底）。
                    if (g_requests.currentProjectId() != projectId) {
                        (void)g_requests.selectProject(projectId);
                        (void)g_loadtest.setProject(projectId);
                    }
                    break;
                case TopTabKind::GlobalSettings:
                    // 子窗口没有设置标签，防御路由到主窗口。
                    windowManager->WakeMainToSettings();
                    break;
            }
        },
        [windowManager, projectId](const TopTabId target) {
            if (target.kind == TopTabKind::Project) windowManager->CloseChildTab(projectId);
        },
        [windowManager, navPage, projectId](
            std::int64_t, const huxerui::Point&) {
            windowManager->MergeBack(projectId, projectId, navPage.Get());
        },
    };

    const int themeModeValue = themeMode.Get();
    const bool dark =
        themeModeValue == 1 || (themeModeValue == 0 && cfg::systemPrefersDark());
    const huxerui::ThemeSpec rootSpec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const float gap = rootSpec.spacing.extra_small;
    const float titleBarHorizontalPadding =
        compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small;
    const float sideShellWidth =
        compact ? kCompactSideShellWidth : kRegularSideShellWidth;
    const float titleBarLogoWidth = sideShellWidth - 2.0F * titleBarHorizontalPadding;

    // 内容区：子窗口只有项目工作区（无主页/设置页），navPage 表达内部页。
    huxerui::View page = pages::PageCard(pages::ProjectPages(navPage, activeProject, projectId), true)
                             .Key(projectId)
                             .With(huxerui::Grow(1.0F));

    huxerui::View content = huxerui::Column {
        huxerui::WindowTitleBar {
            TitleBarLogo(titleBarLogoWidth).With(huxerui::WindowDragRegion{}),
            TopTabStrip(tabs, settingsOpen, activeTopTab, topTabActions, true),
            huxerui::Spacer{}.With(huxerui::Grow(1.0F), huxerui::WindowDragRegion{}),
            // 齿轮 = 唤醒主窗口打开全局设置（与主窗口同款外观，行为路由主窗口）。
            huxerui::Row {
                huxerui::Image(app::images::gear)
                    .Fit(huxerui::ImageFit::Contain)
                    .Align(huxerui::HorizontalAlignment::Center,
                           huxerui::VerticalAlignment::Center)
                    .Tint(rootSpec.colors.on_surface)
                    .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                      huxerui::Frame{.height = kTitleBarContentHeight},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Tooltip("全局设置（主窗口）"),
                      huxerui::Focusable(true),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = "全局设置"})
                .OnClick([windowManager] { windowManager->WakeMainToSettings(); }),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      titleBarHorizontalPadding, 0.0F)),
                  huxerui::Spacing(gap)),
        huxerui::Row {
            huxerui::View{SideShell(navPage)},
            std::move(page).With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
    }
                               .With(huxerui::Background(rootSpec.colors.background),
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return MinimalThemed(dark, std::move(content));
}

} // namespace apitab::ui
