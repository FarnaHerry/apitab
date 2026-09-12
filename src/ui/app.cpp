// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘）：
//   标题栏岛：Logo(AT) + 顶级标签条（TopTabStrip：主页钉在最左、项目标签横向滚动、
//     设置单例标签固定追加在所有项目标签之后）+ 齿轮（全局设置单例标签）+
//     框架窗口按钮；收窄为 24px 高，使用轻量玻璃背景融入窗口底色，
//     主题为 apitab 海洋品牌风：深海蓝、冰川青、玻璃白与柔和的水母紫点缀。
//   下方：左侧图标侧边栏（独立导航岛）｜内容区（页面自己的一级岛屿划分区域，
//   外壳不再套岛）。根节点刷整窗底色（rootSpec.colors.background——
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
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
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

[[huxerui::composable]] huxerui::View PageFor(std::size_t index,
                                              huxerui::State<std::int64_t> activeProject) {
    switch (index) {
        case kLoad:
            return LoadTestPage();
        case kHistory:
            return HistoryPage();
        case kProjectSettings:
            return ProjectSettingsPage();
        case kRequest:
        default:
            // 主页已整宽覆盖侧栏：未打开项目时本页不可达，无需兜底。
            return RequestPage(activeProject);
    }
}
} // namespace pages

namespace {

// 标题栏几何常量与 TopTabDisplayKey 已移至 ui.h / title_bar.cpp（P1-C2 纯搬移）：
// kTitleBarContentHeight / kProjectTabWidth / kSettingsTabDisplayKey 见 ui.h；
// TopTabDisplayKey / ProjectTabDragPayload / TopTab / TopTabStrip / LogoBadge 见 title_bar.cpp。

// apitab 水母品牌主题：颜色先落到语义 token，再由 typed style 统一消费。
// 这组值对应参考图的浅色/深色设计板；页面不应再直接散落品牌色。
huxerui::ThemeSpec OceanDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 26.0F,
    };
    spec.shapes = huxerui::ShapeScheme{.extra_small = 4.0F,
                                       .small = 6.0F,
                                       .medium = 8.0F,
                                       .large = 12.0F,
                                       .extra_large = 18.0F,
                                       .full = 10000.0F};
    spec.spacing = huxerui::SpacingScheme{.extra_small = 4.0F,
                                          .small = 8.0F,
                                          .medium = 12.0F,
                                          .large = 16.0F,
                                          .extra_large = 24.0F};
    spec.motion.fast = 0.16;
    spec.motion.normal = 0.20;
    spec.motion.slow = 0.30;
    spec.colors.primary = huxerui::Color::Rgb(67, 211, 220);          // #43D3DC
    spec.colors.on_primary = huxerui::Color::Rgb(7, 30, 35);
    spec.colors.primary_container = huxerui::Color::Rgb(21, 52, 61);  // #15343D
    spec.colors.on_primary_container = huxerui::Color::Rgb(234, 247, 248);
    spec.colors.secondary = huxerui::Color::Rgb(157, 187, 192);       // #9DBBC0
    spec.colors.on_secondary = huxerui::Color::Rgb(7, 21, 27);
    spec.colors.secondary_container = huxerui::Color::Rgb(16, 42, 51); // #102A33
    spec.colors.on_secondary_container = huxerui::Color::Rgb(234, 247, 248);
    spec.colors.tertiary_container = huxerui::Color::Rgb(26, 63, 74);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(191, 239, 242);
    spec.colors.background = huxerui::Color::Rgb(7, 21, 27);         // #07151B
    spec.colors.surface = huxerui::Color::Rgb(12, 32, 40);            // #0C2028
    spec.colors.surface_container_low = huxerui::Color::Rgb(16, 42, 51); // #102A33
    spec.colors.surface_container = huxerui::Color::Rgb(21, 52, 61); // #15343D
    spec.colors.surface_container_high = huxerui::Color::Rgb(26, 62, 72);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(21, 52, 61); // elevated
    spec.colors.on_surface = huxerui::Color::Rgb(234, 247, 248);     // #EAF7F8
    spec.colors.on_surface_variant = huxerui::Color::Rgb(157, 187, 192);
    spec.colors.outline = huxerui::Color::Rgb(33, 67, 75);            // #21434B
    spec.colors.inverse_surface = huxerui::Color::Rgb(234, 247, 248);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(7, 21, 27);
    spec.colors.error = huxerui::Color::Rgb(255, 142, 134);
    spec.interactions.focus_ring = huxerui::FocusRing{spec.colors.primary, 2.0F, 2.0F};
    return spec;
}

huxerui::ThemeSpec OceanLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    spec.typography = huxerui::TypographyScheme{
        .body_large = 16.0F,
        .body_medium = font_size::kBody,
        .body_small = font_size::kChip,
        .label_large = font_size::kBody,
        .title_large = font_size::kTitle,
        .headline_small = 26.0F,
    };
    spec.shapes = huxerui::ShapeScheme{.extra_small = 4.0F,
                                       .small = 6.0F,
                                       .medium = 8.0F,
                                       .large = 12.0F,
                                       .extra_large = 18.0F,
                                       .full = 10000.0F};
    spec.spacing = huxerui::SpacingScheme{.extra_small = 4.0F,
                                          .small = 8.0F,
                                          .medium = 12.0F,
                                          .large = 16.0F,
                                          .extra_large = 24.0F};
    spec.motion.fast = 0.16;
    spec.motion.normal = 0.20;
    spec.motion.slow = 0.30;
    spec.colors.primary = huxerui::Color::Rgb(40, 184, 199);          // #28B8C7
    spec.colors.on_primary = huxerui::Color::Rgb(7, 52, 58);
    spec.colors.primary_container = huxerui::Color::Rgb(205, 239, 242); // #CDEFF2
    spec.colors.on_primary_container = huxerui::Color::Rgb(24, 52, 58);
    spec.colors.secondary = huxerui::Color::Rgb(96, 122, 128);       // #607A80
    spec.colors.on_secondary = huxerui::Color::White();
    spec.colors.secondary_container = huxerui::Color::Rgb(239, 249, 250); // #EFF9FA
    spec.colors.on_secondary_container = huxerui::Color::Rgb(24, 52, 58);
    spec.colors.tertiary_container = huxerui::Color::Rgb(224, 246, 247);
    spec.colors.on_tertiary_container = huxerui::Color::Rgb(35, 82, 90);
    spec.colors.background = huxerui::Color::Rgb(247, 252, 253);     // #F7FCFD
    spec.colors.surface = huxerui::Color::White();                    // #FFFFFF
    spec.colors.surface_container_low = huxerui::Color::Rgb(239, 249, 250); // #EFF9FA
    spec.colors.surface_container = huxerui::Color::Rgb(244, 250, 251);
    spec.colors.surface_container_high = huxerui::Color::Rgb(230, 246, 247);
    spec.colors.surface_container_highest = huxerui::Color::White();
    spec.colors.on_surface = huxerui::Color::Rgb(24, 52, 58);         // #18343A
    spec.colors.on_surface_variant = huxerui::Color::Rgb(96, 122, 128);
    spec.colors.outline = huxerui::Color::Rgb(217, 234, 236);         // #D9EAEC
    spec.colors.inverse_surface = huxerui::Color::Rgb(24, 52, 58);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(247, 252, 253);
    spec.colors.error = huxerui::Color::Rgb(190, 65, 78);
    spec.interactions.focus_ring = huxerui::FocusRing{spec.colors.primary, 2.0F, 2.0F};
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式，
// 让普通控件、输入框和弹出层共享设计稿的圆角与玻璃表面。
huxerui::View OceanThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? OceanDarkThemeSpec() : OceanLightThemeSpec();
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);
    const auto withAlpha = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };

    huxerui::ButtonStyle buttons = huxerui::ButtonStyle::Default();
    buttons.background = spec.colors.primary;
    buttons.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                             spec.colors.on_primary};
    buttons.disabled_background = withAlpha(spec.colors.on_surface, 0.10F);
    buttons.disabled_label = withAlpha(spec.colors.on_surface, 0.42F);
    buttons.padding = huxerui::EdgeInsets::Symmetric(16.0F, 9.0F);
    buttons.minimum_height = 36.0F;
    buttons.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    buttons.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.10F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.18F)},
    };
    definition.Set(buttons);

    huxerui::IconButtonStyle icons = huxerui::IconButtonStyle::Default();
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

    huxerui::SegmentedButtonStyle segments = huxerui::SegmentedButtonStyle::Default();
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.primary;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_primary;
    segments.border = huxerui::Border{spec.colors.outline, 1.0F};
    segments.selected_border = huxerui::Border{spec.colors.primary, 1.0F};
    segments.padding = huxerui::EdgeInsets::Symmetric(12.0F, 6.0F);
    segments.minimum_height = 32.0F;
    segments.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    definition.Set(segments);

    huxerui::ChipStyle chips = huxerui::ChipStyle::Default();
    chips.background = spec.colors.surface_container_low;
    chips.selected_background = spec.colors.primary_container;
    chips.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kChip),
                                           spec.colors.on_surface_variant};
    chips.selected_label = spec.colors.on_primary_container;
    chips.border = huxerui::Border{spec.colors.outline, 1.0F};
    chips.selected_border = huxerui::Border{spec.colors.primary, 1.0F};
    chips.padding = huxerui::EdgeInsets::Symmetric(10.0F, 4.0F);
    chips.minimum_height = 24.0F;
    chips.corner_radii = huxerui::CornerRadii{spec.shapes.full};
    definition.Set(chips);

    huxerui::TabsStyle tabs = huxerui::TabsStyle::Default();
    tabs.background = huxerui::Color::Transparent();
    tabs.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                          spec.colors.on_surface_variant};
    tabs.selected_label = spec.colors.on_surface;
    tabs.indicator = spec.colors.primary;
    tabs.indicator_height = 2.0F;
    tabs.indicator_corner_radius = 1.0F;
    tabs.divider_color = huxerui::Color::Transparent();
    tabs.divider_height = 0.0F;
    tabs.item_padding = huxerui::EdgeInsets::Symmetric(12.0F, 6.0F);
    tabs.minimum_height = 32.0F;
    tabs.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.primary, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.primary, 0.12F)},
    };
    tabs.indicator_animation_duration = spec.motion.normal;
    definition.Set(tabs);

    huxerui::TreeViewStyle tree = huxerui::TreeViewStyle::Default();
    tree.background = huxerui::Color::Transparent();
    tree.foreground = spec.colors.on_surface;
    tree.disabled_foreground = withAlpha(spec.colors.on_surface, 0.42F);
    tree.selected_background = withAlpha(spec.colors.primary, 0.12F);
    tree.active_background = withAlpha(spec.colors.primary, 0.06F);
    tree.focus_indicator = spec.colors.primary;
    tree.item_extent = 32.0F;
    tree.item_padding = 6.0F;
    tree.indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.10F)},
    };
    definition.Set(tree);

    huxerui::DividerStyle divider = huxerui::DividerStyle::Default();
    divider.color = spec.colors.outline;
    divider.thickness = 1.0F;
    definition.Set(divider);

    huxerui::TextFieldStyle fields = huxerui::TextFieldStyle::Default();
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
    for (huxerui::TextFieldVariantStyle* variant : {&fields.standard, &fields.filled,
                                                    &fields.outlined}) {
        variant->background = spec.colors.surface_container_highest;
        variant->disabled_background = withAlpha(spec.colors.on_surface, 0.06F);
        variant->border = spec.colors.outline;
        variant->hovered_border = spec.colors.secondary;
        variant->focused_border = spec.colors.primary;
        variant->disabled_border = withAlpha(spec.colors.on_surface, 0.18F);
        variant->minimum_height = 36.0F;
        variant->corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    }
    fields.standard.background = huxerui::Color::Transparent();
    fields.filled.background = spec.colors.surface_container_low;
    fields.outlined.background = spec.colors.surface;
    fields.padding = huxerui::EdgeInsets::Symmetric(10.0F, 7.0F);
    definition.Set(fields);

    // 内置确认框（DialogHandle::Show(title, message, ...) 形态）跟随主题：DialogStyle
    // 是 Environment 值（presentation.h 有 Default()/==），经 ThemeDefinition::Set
    // 全局覆盖。Default() 基线是白底浅色配色，逐字段换色。
    // 删除/清空等破坏性确认已统一走 ui 层 ShowDangerConfirm（确认按钮染 error 红），
    // 这里的覆盖保留为内置形态的兜底主题。
    // 叠加层（hover/press）改用 on_surface/on_primary 派生的半透明色，
    // 保持深浅主题和玻璃表面的对比度。
    huxerui::DialogStyle dialogs = huxerui::DialogStyle::Default();
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
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.10F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_primary, 0.18F)},
    };
    dialogs.negative_action_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                                       spec.colors.on_surface};
    dialogs.negative_action_indication = huxerui::Indication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.06F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    dialogs.action_separator_color = spec.colors.outline;
    dialogs.corner_radii = huxerui::CornerRadii{
        spec.shapes.large + spec.spacing.extra_small * 0.5F};
    dialogs.action_corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    definition.Set(dialogs);

    // 下拉选择（Select，全局设置页/历史页在用）跟随主题：触发框与弹出菜单
    // 使用中等圆角，和请求参数行、卡片保持同一视觉节奏。
    // 叠加层沿用 DialogStyle 的 on_surface 半透明做法，不用 M3 ripple。
    huxerui::SelectStyle selects = huxerui::SelectStyle::Default();
    selects.background = spec.colors.surface_container_highest;
    selects.foreground = spec.colors.on_surface;
    selects.border = huxerui::Border{spec.colors.outline, 1.0F};
    selects.indicator = spec.colors.on_surface_variant;
    selects.popup_background = spec.colors.surface_container;
    selects.active_item_background = withAlpha(spec.colors.primary, 0.08F);
    selects.selected_item_background = withAlpha(spec.colors.primary, 0.12F);
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
    huxerui::MenuStyle menus = huxerui::MenuStyle::Default();
    menus.background = spec.colors.surface_container;
    menus.foreground = spec.colors.on_surface;
    menus.icon_tint = spec.colors.on_surface_variant;
    menus.separator_color = spec.colors.outline;
    menus.shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.16F), {}, 12.0F, 0.0F};
    menus.corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    menus.item_indication = selectIndication;
    definition.Set(menus);

    huxerui::ComboBoxStyle combos = huxerui::ComboBoxStyle::Default();
    combos.popup_background = spec.colors.surface_container;
    combos.foreground = spec.colors.on_surface;
    combos.active_item_background = withAlpha(spec.colors.primary, 0.08F);
    combos.item_padding = selects.item_padding;
    combos.popup_shadow = selects.popup_shadow;
    combos.minimum_item_height = selects.minimum_item_height;
    combos.maximum_popup_height = selects.maximum_popup_height;
    combos.popup_corner_radii = huxerui::CornerRadii{spec.shapes.medium};
    combos.item_indication = selectIndication;
    definition.Set(combos);

    return huxerui::Theme(std::move(definition), content);
}

// LogoBadge / TopTab / TopTabStrip 已移至 title_bar.cpp（P1-C2 纯搬移），此处保留占位注释。


// 单个顶级标签：激活态 = 最高层级容器底 + 主文字色；未激活 = 略深容器底 + 次级文字色。
// 整块外层只负责激活与切换（点击会卸载内容子树，切换统一经 actions.activate 的
// AppRoot 推迟任务执行，CLAUDE.md 约定 6）；内层用两个兄弟节点分别承载「切换」与
// 「关闭」，避免各自做一次整标签的背景重绘。主页标签（kind=Home）不可关闭、不挂
// 拖拽，位置恒定最左；项目标签（kind=Project）可关闭、挂拖拽换位；设置单例标签
// （kind=GlobalSettings）可关闭、不挂拖拽——拖拽 payload 只接受项目，设置不参与
// 拖拽排序、不写入 open_projects（§13.4 B0.1）。拖拽的 strip 级状态（dragId/dragDx/
// dragOrig）与几何（index/count/stride）由 TopTabStrip 传入：拖动时本标签变透明
// 占位，视觉由条内覆盖层克隆接管。
// TopTab 已移至 title_bar.cpp（P1-C2）

// TopTabStrip 已移至 title_bar.cpp（P1-C2）


// 左列：图标侧边栏（选中态用实心图标变体，悬停显示文字提示）。
// 侧栏是海洋主题里的独立导航岛，选中项只用浅色容器和左侧指示条强调；WebSocket/TCP
// 已并入请求页标签，不再单列。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
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
        Item{app::images::project_settings, app::images::project_settings_selected, "项目设置",
             pages::kProjectSettings},
    };

    std::vector<huxerui::View> buttons;
    for (const Item& item : items) {
        const std::size_t page = item.page;
        const bool selected = navPage.Get() == page;
        const huxerui::ImageResource& icon = selected ? item.icon_selected : item.icon;
        buttons.push_back(
            huxerui::Row{
                huxerui::Row{}
                    .With(huxerui::Frame{.width = 3.0F, .height = 20.0F},
                          huxerui::Background(selected ? theme.colors.primary
                                                        : huxerui::Color::Transparent()),
                          huxerui::CornerRadius(theme.shapes.full)),
                huxerui::IconButton(icon, item.tooltip)
                    .OnClick([tasks, navPage, page] {
                        // 切页会卸载内容子树：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            navPage = page;
                        });
                    }),
            }
                .With(huxerui::Frame{.width = 40.0F, .height = 40.0F},
                      huxerui::Background(selected ? theme.colors.primary_container
                                                    : huxerui::Color::Transparent()),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::Spacing(2.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
                .With(huxerui::Tooltip(item.tooltip)));
    }
    return huxerui::Column(std::move(buttons))
        .With(huxerui::Padding(compact ? theme.spacing.small : theme.spacing.medium),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = compact ? 52.0F : 64.0F},
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 底部状态条已移至 global_status_bar.cpp（P1-C2 纯搬移）

} // namespace

// 关闭询问弹窗宿主：必须在 OceanThemed provider 之下组合——AppRoot 自身在
// provider 之上，层内容捕获调用处环境，在 AppRoot 里 dialog.Show 的弹窗
// UseTheme() 只能拿到默认浅色 spec（弹窗不应用主题的根因）。关闭拦截
// （OnCloseRequest）与询问弹窗都挂在这里；content 原样返回，仅附加行为。
// CloseGuard 已移至 app_dialogs.cpp（P1-C2 纯搬移）

[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    // IsAvailable() 内部会观察托盘可用性 State：DBus 托盘宿主就绪较晚时，
    // 这里在组合期订阅，可用性翻转后本作用域重组、托盘随后注册。
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();
    auto loggedIn = huxerui::UseState(true);
    auto loginDialog = huxerui::UseDialog();
    huxerui::ImageAsset initialAvatar;
    const std::filesystem::path avatarPath = cfg::dataDir() / "avatar.png";
    if (std::filesystem::exists(avatarPath)) {
        try { initialAvatar = huxerui::ImageAsset::FromFile(avatarPath); } catch (const std::exception&) {}
    }
    auto avatarImage = huxerui::UseState(initialAvatar);

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色，未保存偏好时默认深色（深海蓝底）。
    int initialThemeMode = 1;
    if (sessionPreference("theme_mode") == "0") initialThemeMode = 0;
    if (sessionPreference("theme_mode") == "2") initialThemeMode = 2;
    // 关闭行为：0=每次询问 1=直接关闭 2=最小化到托盘
    int initialCloseBehavior = 0;
    if (sessionPreference("close_behavior") == "1") initialCloseBehavior = 1;
    if (sessionPreference("close_behavior") == "2") initialCloseBehavior = 2;

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
    TopTabState restored = [&] {
        std::vector<std::int64_t> existIds;
        for (const db::Project& p : g_requests.allProjects()) existIds.push_back(p.id);
        return RestoreTopTabs(sessionPreference("open_projects"), sessionPreference("active_project"),
                              existIds);
    }();
    // 领域游标与 State 同步（启动时即一致，首帧不闪回主页）。
    if (restored.active.kind == TopTabKind::Project) {
        g_requests.selectProject(restored.active.project_id);
        g_loadtest.setProject(restored.active.project_id);
    } else {
        g_requests.selectProject(0);
        g_loadtest.setProject(0);
    }
    auto tabs = huxerui::UseState(std::move(restored.open_projects));
    auto activeProject = huxerui::UseState(
        restored.active.kind == TopTabKind::Project ? restored.active.project_id : std::int64_t{0});
    auto settingsOpen = huxerui::UseState(restored.settings_open);
    auto activeTopTab = huxerui::UseState(restored.active);
    auto lastProjectTab = huxerui::UseState(restored.last_project);
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    auto closeBehavior = huxerui::UseState<int>(std::move(initialCloseBehavior));
    auto closeDialogOpen = huxerui::UseState(false);
    // P1-B0.5 状态保活：设置分类在 AppRoot，随顶级标签存活（切到项目再回保留原分类）
    auto settingsCategory = huxerui::UseState<std::size_t>(0);

    // ---- 顶级标签操作（事件回调只做 tasks.Launch 推迟，CLAUDE.md 约定 6）----
    // 变更本体（领域写入 + State 写回）在推迟任务里同步完成：切到 Project(id) 时
    // 先做领域写入再写 activeTopTab，同一任务内两者一致、重组无中间帧（§13.2
    // 不变量 1）。*Now 函数只能从推迟语境调用（组合体内禁止写 State）。

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
        g_requests.selectProject(id);
        g_loadtest.setProject(id);
        saveSessionPreference("active_project", std::to_string(id));
        activeProject = id;
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
            syncDomainProject(after.active.project_id);
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
            syncDomainProject(after.active.project_id);
        } else if (target.kind == TopTabKind::Project && before.active == target) {
            g_requests.selectProject(0);
            g_loadtest.setProject(0);
            saveSessionPreference("active_project", "0");
            activeProject = 0;
        }
        commitTopTab(before, after);
    };

    // 事件入口包装：切/关标签会卸载被点节点，推迟出指针事件路径（约定 6）。
    auto activateTopTab = [tasks, activateTopTabNow](TopTabId target) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            activateTopTabNow(target);
        });
    };
    auto closeTopTab = [tasks, closeTopTabNow](TopTabId target) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            closeTopTabNow(target);
        });
    };
    TopTabActions topTabActions{activateTopTab, closeTopTab};

    // HomePage 打开项目回调：ProjectCard 的推迟任务在完成领域写入后调用（仍在
    // 推迟语境），内部走同一 activateTopTabNow——新增/激活顶级项目标签 + State
    // 写回 + open_projects 按需持久化（格式与原 CSV 一致）。
    std::function<void(std::int64_t)> onOpenProject = [activateTopTabNow](std::int64_t id) {
        activateTopTabNow(TopTabId{TopTabKind::Project, id});
    };

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    // 注意：AppRoot 里的 UseTheme() 拿到的是 MaterialTheme provider 之上（应用外）
    // 的默认浅色 spec——主题由本函数返回时包进子树，自身读不到。所以根节点自身的
    // 配色（整窗背景、标题栏底、间距）必须直接按 dark 选 spec；子组件在 provider
    // 之下，它们的 UseTheme() 是正常的。
    const huxerui::ThemeSpec rootSpec = dark ? OceanDarkThemeSpec() : OceanLightThemeSpec();
    // 响应式：Compact(<600) 收窄间距，Medium/Expanded 保持现状。
    // 根 Column 子项间隙统一 extra_small(4pt)：标题栏↔主行贴紧一些。底部状态栏
    // 仅属于项目工作区；主页和通用设置页没有它，主岛直接吃满标题栏以下的高度。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const float gap = compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small;
    const float statusTopPad = gap - rootSpec.spacing.extra_small;

    // 托盘：图标 + 菜单；点击托盘图标激活主窗口。仅在可用时注册；
    // 首次组合时宿主未就绪则跳过，待可用性触发重组后再注册。
    if (trayAvailable) {
        tray.OnActivate([window] { window.Activate(); });
        huxerui::Lifecycle(
            [tray, window, application] {
                // 菜单项用 push_back 构造：GCC 16 对 menu 列表初始化内的
                // MenuItem/MenuSection 隐式转换报 "expected primary-expression"
                std::vector<huxerui::MenuEntry> menuEntries;
                menuEntries.push_back(
                    huxerui::MenuItem("显示主窗口", [window] { window.Activate(); }));
                menuEntries.push_back(huxerui::MenuSection{});
                menuEntries.push_back(
                    huxerui::MenuItem("退出", [application] { application.Quit(); }));
                // SystemTray 当前只接受可解析为 ImageAsset 的栅格资源；tray.png
                // 已替换为新的水母图标，不能直接传 SVG 的 VectorAsset。
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
    indexed.push_back(HomePage(onOpenProject, activeProject));
    std::unordered_map<std::int64_t, std::size_t> projIdx;
    for (std::int64_t id : tabs.Get()) {
        const std::size_t idx = indexed.size();
        projIdx[id] = idx;
        indexed.push_back(pages::PageFor(navPage.Get(), activeProject).Key(id).With(huxerui::Grow(1.0F)));
    }
    if (settingsOpen.Get()) {
        indexed.push_back(GlobalSettingsPage(themeMode, closeBehavior, settingsCategory, avatarImage)
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
    const bool showProjectStatusBar = showSideShell;
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
                return OceanThemed(dark, LoginPage(ctx, loggedIn));
            },
            huxerui::DialogOptions{});
    });

    huxerui::View content = huxerui::Column {
        // 自定义标题栏岛：Logo + 顶级标签条 + 齿轮（框架在其右侧渲染窗口按钮）。
        // 全部内容统一 24pt 高（kTitleBarContentHeight = title_bar_height）；
        // WindowTitleBar 构造即带交叉轴居中，这里给中间标签条包装 Row 也补上
        // 居中，任何一侧偏高都不漂移。
        huxerui::WindowTitleBar {
            LogoBadge().With(huxerui::WindowDragRegion{}),
            // 标签条按真实内容宽度布局；窄窗时受父约束收缩并横向滚动。
            TopTabStrip(tabs, settingsOpen, activeTopTab, topTabActions),
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
            // 齿轮：打开/激活设置单例标签（未开则开、已开仅激活，§13.1）；激活
            // 经 activateTopTab 的推迟任务（约定 6），顶级状态写回见上。
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
                .OnClick([tasks, activateTopTab] {
                    // 切标签会卸载内容子树：推迟出指针事件路径（约定 6）
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        activateTopTab(TopTabId{TopTabKind::GlobalSettings, 0});
                    });
                }),
            // 用户头像：固定圆形命中区；点击打开登录弹窗。
            std::move(topAvatar),
        }
            // 标签栏保持轻量，但使用低层级玻璃表面把品牌导航从内容区托起。
            // 垂直零内边距：内容本身 24pt 高，与 title_bar_height 对齐，避免
            // 标题栏下缘与岛屿之间多出一条空隙。
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small, 0.0F)),
                  huxerui::Spacing(gap),
                  huxerui::Background(rootSpec.colors.surface_container_low),
                  huxerui::Border(rootSpec.colors.outline, 1.0F)),
        // 主行：导航岛 + 内容区；Grow 吃满标题栏之外的剩余高度。
        // 内容区不再套外壳岛：区域划分由各页面自己的一级岛屿承担，避免双层嵌套。
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
            .With(huxerui::Spacing(gap),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
        // 仅项目工作区显示底栏：代理与项目 Cookie 都是项目范围的配置。Home 和
        // GlobalSettings 返回零尺寸占位，主行 Grow 自动填满释放的全部高度。
        showProjectStatusBar ? huxerui::View{GlobalStatusBar(statusTopPad)}
                             : huxerui::View{huxerui::Row{}},
    }
                               .With(huxerui::Spacing(rootSpec.spacing.extra_small),
                                     // 窗口整体背景：主题背景色刷满根节点，
                                     // 否则深色模式下岛间缝隙透出窗口默认白底。
                                     huxerui::Background(rootSpec.colors.background),
                                     // 交叉轴必须 Stretch：否则主行（侧栏+页面区）按内容
                                     // 收缩成内容宽，页面里所有 Grow 失去上界、岛屿无法
                                     // 占满逻辑区块（首页整体漂移/右对齐的根因）。
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 主题边界走 OceanThemed：自定义品牌 spec + 组件 typed style 覆盖。
    // 关闭询问弹窗宿主挂在 provider 之下（AppRoot 自身读不到主题，CloseGuard 能）。
    return OceanThemed(dark, CloseGuard(closeBehavior, closeDialogOpen, std::move(content)));

}

} // namespace apitab::ui
