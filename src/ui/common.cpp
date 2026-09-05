// common.cpp — HuxerUI 前端公共小组件（页面标题、状态文本、空状态页、图标按钮、
// 方法+地址组合栏）。下拉选择一律用官方 Select（view.h），不再自拼菜单触发器。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"
#include "sweetline_provider.h"

import std;

import apitab.utils;

namespace apitab::ui {

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme) {
    return IslandTheme{
        .page_gap = theme.spacing.medium,
        // 一级岛与请求页/主页一致使用 medium 内边距；设置页不能另起一套更厚卡片。
        .island_padding = theme.spacing.medium,
        .island_radius = 16.0F,
        .nested_radius = 8.0F,
        // 几何令牌（ui.h IslandTheme 中段字段）：全项目唯一圆角/图标按钮尺寸
        // 来源，页面不得再散落魔法数字。control/large_control 从主题 ShapeScheme
        // 派生（small=8 / medium=12），命中区两档与控件高为固定常量。
        .control_radius = theme.shapes.small,           // 8pt：普通按钮/选择器/局部控件
        .large_control_radius = theme.shapes.medium,    // 12pt：大输入行/请求组合栏
        .icon_button_compact = 28.0F,                   // 图标按钮紧凑命中区（正方形）
        .icon_button_regular = 32.0F,                   // 图标按钮舒适命中区（正方形）
        .control_height = 32.0F,                        // 普通控件统一高度
        .rail_width = 48.0F,
        .ocean = theme.colors.background,
        .base = theme.colors.surface_container_low,
        .raised = theme.colors.surface_container,
        .active = theme.colors.surface_container_high,
        .overlay = theme.colors.surface_container_highest,
        .outline_soft = theme.colors.outline,
    };
}

float ConcentricRadius(float outer_radius, float inset) {
    // 内层半径 = 外层半径 − inset，保下限 4pt：更小的半径在视觉上与父级
    // 圆角几乎相切，出现反同心（子角比父角"尖"）。
    constexpr float kMinRadius = 4.0F;
    return std::max(outer_radius - inset, kMinRadius);
}

static huxerui::Color IslandColor(const IslandTheme& islands, const huxerui::ThemeSpec& theme,
                                  IslandLevel level) {
    switch (level) {
    case IslandLevel::Base: return islands.base;
    case IslandLevel::Raised: return islands.raised;
    case IslandLevel::Active: return islands.active;
    case IslandLevel::Overlay: return islands.overlay;
    case IslandLevel::Danger: {
        huxerui::Color danger = theme.colors.error;
        danger.alpha = 0.10F;
        return danger;
    }
    }
    return islands.base;
}

[[huxerui::composable]] huxerui::View IslandSurface(huxerui::View content, IslandLevel level) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View surface = content;
    return std::move(surface).With(huxerui::Background(IslandColor(islands, theme, level)),
                                   huxerui::CornerRadius(islands.island_radius),
                                   huxerui::Padding(islands.island_padding));
}

[[huxerui::composable]] huxerui::View IslandSection(std::string title,
                                                    std::string description,
                                                    huxerui::View content) {
    std::vector<huxerui::View> children;
    children.reserve(description.empty() ? 2 : 3);
    children.push_back(huxerui::Text(std::move(title), huxerui::TextRole::Title));
    if (!description.empty())
        children.push_back(huxerui::Text(std::move(description), huxerui::TextRole::Body)
                               .With(huxerui::Foreground(
                                   huxerui::UseTheme().colors.on_surface_variant)));
    children.push_back(content);
    return IslandSurface(
        huxerui::Column(std::move(children))
            .With(huxerui::Spacing(12.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        IslandLevel::Base);
}

[[huxerui::composable]] huxerui::View IslandDialog(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F},
        huxerui::Background(islands.overlay), huxerui::CornerRadius(islands.island_radius),
        huxerui::Border(islands.outline_soft, 1.0F), huxerui::ClipChildren(),
        huxerui::Padding(islands.island_padding));
}

namespace {

// 声明式 Rotation 没有暂停/续播接口，改由节点保存光圈相位。
struct AvatarRing {
    class Extension;
    float size;
    float width;
    bool flowing;
    bool operator==(const AvatarRing&) const = default;
};

class AvatarRing::Extension final : public huxerui::NodeExtension {
public:
    Extension(huxerui::MountedNode& node, const AvatarRing& value) { Update(node, value); }

    void Update(huxerui::MountedNode&, const AvatarRing& value) {
        if (value.flowing != value_.flowing) firstFrame_ = true;
        value_ = value;
        InvalidatePaint();
    }

    FrameResult OnFrame(huxerui::MountedNode&, const huxerui::FrameInfo& frame) override {
        if (!value_.flowing || frame.reduced_motion) {
            firstFrame_ = true;
            return {};
        }
        // 续播首帧不累计悬停前的时间，角度始终从最后绘制的位置继续。
        if (firstFrame_) {
            firstFrame_ = false;
        } else {
            constexpr double kTurn = 2.0 * std::numbers::pi;
            angle_ = std::fmod(angle_ + frame.delta_time * kTurn / 2.4, kTurn);
            InvalidatePaint();
        }
        return {.needs_frame = true};
    }

    void PaintAboveContent(const huxerui::MountedNode&, huxerui::PaintContext& paint) const override {
        const float halfStroke = value_.width * 0.5F;
        const auto circle = huxerui::Path::RoundedRect(
            {halfStroke, halfStroke, value_.size - value_.width, value_.size - value_.width},
            huxerui::CornerRadii{value_.size * 0.5F});
        const float cosine = static_cast<float>(std::cos(angle_));
        const float sine = static_cast<float>(std::sin(angle_));
        const float dx = 0.35F * (cosine + sine);
        const float dy = 0.35F * (sine - cosine);
        paint.StrokePath(circle, huxerui::LinearGradient{
            .start = {0.5F - dx, 0.5F - dy}, .end = {0.5F + dx, 0.5F + dy},
            .stops = {{0.0F, huxerui::Color::Rgb(48, 128, 255)},
                      {0.6F, huxerui::Color::Rgb(48, 128, 255)},
                      {1.0F, huxerui::Color::Rgb(177, 159, 255)}},
        }, huxerui::StrokeStyle{.width = value_.width});
    }

private:
    AvatarRing value_{};
    double angle_ = 0.0;
    bool firstFrame_ = true;
};

} // namespace

huxerui::View ProfileAvatar(huxerui::ImageAsset image, float size, const huxerui::ThemeSpec& theme,
                            bool hovered) {
    const float ringWidth = size >= 48.0F ? size * (3.0F / 64.0F) : 1.5F;
    const float inset = ringWidth + (size >= 48.0F ? size / 32.0F : 1.0F);
    const float imageSize = size - inset * 2.0F;
    const auto textRole = size >= 48.0F ? huxerui::TextRole::Title : huxerui::TextRole::Label;
    huxerui::View portrait = image.HasValue()
        ? huxerui::View{huxerui::Image(image).Fit(huxerui::ImageFit::Cover)
                            .With(huxerui::Frame{.width = imageSize, .height = imageSize})}
        : huxerui::View{huxerui::Text("U", textRole)
                            .With(huxerui::Foreground(theme.colors.on_primary))};
    huxerui::View ring = huxerui::Stack {}.With(
        huxerui::Frame{.width = size, .height = size},
        AvatarRing{size, ringWidth, hovered && !theme.motion.reduced_motion});
    return huxerui::Stack {
      ring,
      huxerui::Stack { portrait }.With(
          huxerui::Frame{.width = imageSize, .height = imageSize},
          huxerui::Background(theme.colors.primary), huxerui::CornerRadius(imageSize * 0.5F),
          huxerui::ClipChildren(),
          huxerui::Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center)),
    }.With(huxerui::Frame{.width = size, .height = size}, huxerui::CornerRadius(size * 0.5F),
           huxerui::Align(huxerui::HorizontalAlignment::Center, huxerui::VerticalAlignment::Center));
}

// 统一图标动作：glyph 只负责绘制（固定 kIconGlyph 字号，不反向决定按钮大小），
// 命中区固定 icon_button_compact/icon_button_regular 两档（size 只认 28/32，
// 其他值就近收敛：>=30 归 regular，否则 compact）；圆形/圆角方形由 shape 显式
// 选择。所有形状（含 Bare）的 hover/press indication 都覆盖整个命中区——
// indication 以按钮 View 为宿主，fill 按 host corner radii 裁剪，天然是整块命中
// 区，不是字形本身。实现统一挂显式 indication：tint = accent ? on_primary :
// on_surface（6% hover / 12% press，深浅主题自动适配）——不依赖 OnClick 自动
// 追加的 DefaultIndication（其回落到主题 MaterialIndication 按 primary 取色，
// 在 accent=true 的 primary 底上不可见）。注意 SDK 替换规则（view.cpp
// AddModifier）：显式 Indication 会擦除 DefaultIndication、DefaultIndication 遇
// 任何已有 indication 即跳过——**不能用空 Indication{} 表达"用默认"**，那等于
// 整体关掉反馈（P1-A4 审计修正：原先非 Bare 传 Indication{} 导致圆形/圆角方形
// 无 hover/press 反馈）。
// Tooltip（§5.4）：semanticLabel 同时作为 Tooltip 文本，官方 Tooltip modifier
// 挂在按钮 View 上即整块命中区触发；服务在框架根自动装配，TooltipStyle 随主题
// （inverse_surface 底）自动解析，无需调用方接线。禁用态 Tooltip 仍可悬停查看
// （官方 gallery 同款用法），"更多操作"等语义标签即悬浮说明。
[[huxerui::composable]] huxerui::View AppIconButton(std::string glyph,
                                                    std::string semanticLabel,
                                                    std::function<void()> onClick,
                                                    AppIconButtonShape shape, float size,
                                                    bool accent, bool enabled) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool regular = size >= (islands.icon_button_compact + islands.icon_button_regular) / 2.0F;
    const float target = regular ? islands.icon_button_regular : islands.icon_button_compact;
    const bool bare = shape == AppIconButtonShape::Bare;
    const float radius = shape == AppIconButtonShape::Circular ? theme.shapes.full
                                                               : islands.control_radius;
    // 交互反馈：hover/press 覆盖整块命中区；tint 随底色角色取色（accent=主色底
    // 配 on_primary 叠加，其余配 on_surface 叠加），Bare 常态透明同 tint。
    huxerui::Color tint = accent ? theme.colors.on_primary : theme.colors.on_surface;
    huxerui::Color hoverTint = tint;
    hoverTint.alpha = 0.06F;
    huxerui::Color pressTint = tint;
    pressTint.alpha = 0.12F;
    huxerui::Indication hitIndication;
    hitIndication.hover = huxerui::IndicationLayer{
        .fill = huxerui::VisualFill{huxerui::Brush{hoverTint}}};
    hitIndication.press = huxerui::IndicationLayer{
        .fill = huxerui::VisualFill{huxerui::Brush{pressTint}}};
    return huxerui::Row {
        huxerui::Text(std::move(glyph), huxerui::TextRole::Label)
            .With(huxerui::FontSize(font_size::kIconGlyph),
                  huxerui::Foreground(accent ? theme.colors.on_primary
                                             : theme.colors.on_surface)),
    }
        .With(huxerui::Frame{.width = target, .height = target},
              huxerui::Background(bare ? huxerui::Color::Transparent()
                                       : accent ? theme.colors.primary
                                                : theme.colors.surface_container_highest),
              huxerui::CornerRadius(radius),
              std::move(hitIndication),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                  .label = semanticLabel},
              huxerui::Tooltip(semanticLabel),
              // 键盘焦点（P1-B0.4，island-structure-theme.md §13.6）：自定义 Row
              // 默认不可聚焦（仅 Button/IconButton/Chip 等内置 spec 自带
              // focusable=true，view.cpp Make*Spec），Enter/Space 激活要求节点
              // 可聚焦且有 Click 绑定（runtime.cpp IsActivatable +
              // CollectFocusableNodes 收集 enabled && focusable）。补上本修饰符
              // 后 enabled 的图标按钮进入 Tab 序、键盘可激活——补齐 §十
              // "图标按钮…均具有…焦点态" 的最后一项。注意 disabled 节点仍不
              // 参与遍历（如顶级标签 ✕ 的悬停门控，见 app.cpp TopTab）。
              huxerui::Focusable(true),
              huxerui::Enabled(enabled))
        .OnClick(std::move(onClick));
}

// 列表行尾部固定动作区：槽位固定 icon_button_regular 档（32×32 正方形）、
// 间距 4pt；固定 Frame 宽 = 右对齐且槽宽不随 glyph/标签内容抖动。
// 空 actions 返回零宽占位（调用方按列表整体决定是否保留占位）。
[[huxerui::composable]] huxerui::View TrailingActionGroup(std::vector<huxerui::View> actions) {
    const IslandTheme islands = ResolveIslandTheme(huxerui::UseTheme());
    constexpr float kSlotGap = 4.0F; // 槽间距（图标按钮共享 32pt 节奏，留 4pt 视觉缝）
    if (actions.empty()) return huxerui::Row{};
    const float groupWidth = static_cast<float>(actions.size()) * islands.icon_button_regular +
                             static_cast<float>(actions.size() - 1) * kSlotGap;
    return huxerui::Row(std::move(actions))
        .With(huxerui::Spacing(kSlotGap),
              huxerui::Frame{.width = groupWidth, .height = islands.control_height},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// "⋮" 语义图标按钮：AppIconButton Bare + compact 档，只封装菜单触发回调，
// 不引入新菜单数据模型（菜单内容/弹出方式由调用方决定，request_page 的
// RowMenuButton 是参照实现）。禁用时点击空转、整体降透明提示不可用。
[[huxerui::composable]] huxerui::View OverflowButton(std::function<void()> onOpenMenu,
                                                     bool enabled) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 使用中点而不是基线省略号，确保三点在方形命中区内水平、垂直视觉居中。
    return AppIconButton("···", "更多操作", std::move(onOpenMenu), AppIconButtonShape::Bare,
                         28.0F, false, enabled)
        .With(huxerui::Opacity(enabled ? 1.0F : 0.4F),
              huxerui::Foreground(theme.colors.on_surface_variant));
}

// 大型自适应输入表面（契约见 ui.h）。实现要点：
// - 单行↔多行的唯一几何差异是圆角（full capsule ↔ large_control_radius）；
//   Padding/Spacing/尾部动作锚点/ScrollView 结构两态完全一致 → 零跳动。
// - 焦点自动跟踪：把 ViewEvents::FocusChanged 挂在 body 上（body 直接是输入
//   控件时生效；聚焦态写 State 只改描边色，子树结构不变、不会卸载焦点节点）。
//   body 无法上报焦点时由调用方传 InputSurfaceTone::Focused 显式点亮。
// - 正文包在垂直 ScrollView 里并用 Frame 钳制 [min, max] 高度：内容不足按内容
//   收缩、超过 max 后仅正文滚动（ScrollView 内容不足时收缩的语义与菜单层一致，
//   见上方 PopupMenuContent 注释）。单行态同样包 ScrollView（内容恒不足视口，
//   不会滚动），保证切换时树结构稳定。
// - 修饰符顺序 = 由外到内：Background/CornerRadius/Border/ClipChildren 在外，
//   Padding 最内（同 DialogCard 惯例），ClipChildren 保证内容裁进圆角轮廓。
[[huxerui::composable]] huxerui::View AdaptiveInputSurface(huxerui::View body,
                                                           huxerui::View trailing,
                                                           bool multiline,
                                                           InputSurfaceTone tone,
                                                           float minBodyHeight,
                                                           float maxBodyHeight) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto focused = huxerui::UseState(false);
    // hcg codegen 把 composable 体包进 [=] lambda（参数在体内为 const）：
    // 挂焦点事件/钳制高度都需要先拷到局部变量再改写。
    huxerui::View input = std::move(body);
    input = std::move(input).On<huxerui::ViewEvents::FocusChanged>(
        [focused](bool value) { focused = value; });

    // tone → 描边色（优先级 Disabled > Error > Busy > 聚焦 > Normal）。
    // 只换颜色：描边宽度、圆角、内边距在任何 tone 下完全一致。
    huxerui::Color stroke = islands.outline_soft;
    switch (tone) {
    case InputSurfaceTone::Normal:
        if (focused.Get()) stroke = theme.colors.primary;
        break;
    case InputSurfaceTone::Focused:
        stroke = theme.colors.primary;
        break;
    case InputSurfaceTone::Error:
        stroke = theme.colors.error;
        break;
    case InputSurfaceTone::Disabled:
        stroke = islands.outline_soft;
        stroke.alpha *= 0.5F;
        break;
    case InputSurfaceTone::Busy:
        stroke = theme.colors.primary;
        stroke.alpha = 0.55F;
        break;
    }

    // 正文区域：高度钳制 + 拉伸填满剩余宽度（尾部动作整体右对齐）。
    const float minBody = minBodyHeight;
    const float maxBody = std::max(minBodyHeight, maxBodyHeight);
    huxerui::View bodyArea =
        huxerui::ScrollView{huxerui::Column{std::move(input)}.With(huxerui::CrossAlign(
                                huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::Frame{.min_height = minBody, .max_height = maxBody},
                  huxerui::Grow(1.0F));

    constexpr float kPaddingH = 14.0F; // 水平内边距：两态一致（胶囊圆帽不挤正文）
    constexpr float kPaddingV = 6.0F;  // 垂直内边距：两态一致
    constexpr float kActionGap = 8.0F; // 正文与尾部动作间距：两态一致
    return huxerui::Row{std::move(bodyArea), std::move(trailing)}
        .With(huxerui::Spacing(kActionGap),
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(multiline ? islands.large_control_radius
                                              : theme.shapes.full),
              huxerui::Border(stroke, 1.0F),
              huxerui::ClipChildren(),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(kPaddingH, kPaddingV)),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 自定义内容弹窗的卡片包裹：SDK 的 dialog.Show(ViewFactory/DialogFactory) 不给
// 内容加底板（只有标题+消息的内置形态才有 DialogStyle），裸内容直接浮在页面上
// 分不清层级。统一包一层：圆角底 + 阴影 + 内边距；阴影/底/圆角依次在外，
// Padding 在最内（modifier 顺序 = 由外到内）。
[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    return IslandDialog(content);
}

// 危险确认弹窗内容（ShowDangerConfirm 的 DialogFactory 目标）：标题 + 消息 +
// 按钮行（左取消右确认）。确认按钮染红走 ProvideEnvironment 局部覆盖 ButtonStyle
// ——Button 无单实例样式 API（样式经 ResolveStyleOverride<ButtonStyle> 解析），
// Environment 是最窄机制；ColorScheme 没有 on_error 令牌，红底上文字固定白色。
[[huxerui::composable]] huxerui::View DangerConfirmContent(
    huxerui::DialogContext ctx, std::string title, std::string message,
    std::string confirmLabel, std::function<void()> onConfirm) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::ButtonStyle danger = huxerui::UseEnvironment<huxerui::ButtonStyle>();
    danger.background = theme.colors.error;
    danger.label_style.foreground = huxerui::Color::White();
    return DialogCard(huxerui::Column {
        huxerui::Text(std::move(title), huxerui::TextRole::Title),
        huxerui::Text(std::move(message), huxerui::TextRole::Body),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::ProvideEnvironment(
                danger, huxerui::View{huxerui::Button(std::move(confirmLabel))
                                          .OnClick([ctx, onConfirm = std::move(onConfirm)] {
                                              ctx.Dismiss();
                                              onConfirm();
                                          })}),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

void ShowDangerConfirm(huxerui::DialogHandle dialog, std::string title, std::string message,
                       std::string confirmLabel, std::function<void()> onConfirm) {
    dialog.Show(
        [title = std::move(title), message = std::move(message),
         confirmLabel = std::move(confirmLabel),
         onConfirm = std::move(onConfirm)](huxerui::DialogContext ctx) -> huxerui::View {
            return DangerConfirmContent(ctx, title, message, confirmLabel, onConfirm);
        },
        huxerui::DialogOptions{});
}

huxerui::codeeditor::EditorTheme EditorTheme(const huxerui::ThemeSpec& theme) {
    huxerui::codeeditor::EditorTheme editor =
        huxerui::codeeditor::EditorTheme::FromThemeSpec(theme);
    // 行号栏与输入区使用同一表面，去掉中间的硬分隔，形成连续编辑画布。
    editor.gutter_background = editor.background;
    editor.separator_color = huxerui::Color::Transparent();
    return editor;
}

void ConfigureEditorMenu(huxerui::codeeditor::EditorOptions& options) {
    options.context_menu_items = [readOnly = options.read_only] {
        using Item = huxerui::codeeditor::EditorContextItem;
        std::vector<Item> items;
        if (!readOnly) items.push_back({"剪切", 0});
        items.push_back({"复制", 1});
        if (!readOnly) items.push_back({"粘贴", 2});
        items.push_back({"全选", 3});
        items.push_back({"查找", 4});
        items.push_back({"全部折叠", 5});
        items.push_back({"全部展开", 6});
        return items;
    };
}

void ApplyEditorTypography(huxerui::codeeditor::EditorOptions& options) {
    // 空 family 统一使用 HUI 的平台等宽字体，同时避免依赖未随应用打包的字体文件。
    options.font_family.clear();
    options.font_size = editor_metrics::kFontSize;
    options.line_spacing_add = editor_metrics::kLineSpacingAdd;
    options.line_spacing_mult = editor_metrics::kLineSpacingMult;
    options.content_start_padding = editor_metrics::kContentStartPadding;
    options.scrollbar_thickness = editor_metrics::kScrollbarThickness;
}

std::shared_ptr<huxerui::codeeditor::EditorDecorationProvider> SweetLineProvider(
    std::string syntax, std::string initialText, std::string documentKey) {
    static std::unordered_map<std::string,
                              std::shared_ptr<huxerui::codeeditor::EditorDecorationProvider>> cache;
    auto [entry, inserted] = cache.try_emplace(documentKey);
    if (inserted) {
        entry->second = std::make_shared<demo::SweetLineDecorationProvider>(
            std::move(syntax), std::move(initialText), documentKey,
            demo::SweetLineDecorationProvider::GutterIconSource{},
            demo::SweetLineDecorationProvider::PhantomSource{});
    }
    return entry->second;
}

// ---- 自绘弹出菜单（ShowPopupMenu[At]）----
// 作者口径：通用 MenuItem 不支持 per-item 配色/hover 定制，需要就自己用
// UsePopup 做菜单内容。外观对齐环境 MenuStyle（底板/阴影/圆角/内边距/最小宽、
// 条目 hover 用 item_indication）；选中项不用对钩，直接填充比 hover 深一档的
// 底色（取 item_indication 的 press 填充色，兜底 surface_container_highest）；
// 危险项按 PopupMenuDanger 取 error 红（kHoverRed 由逐条 Hover 事件驱动，仅
// hover 时变红）；点击先关闭整链再回调（同系统菜单语义，回调脱离指针路径，
// 但仍需自行推迟会卸载节点的 State 写）；条目超限时限高内部滚动（带
// ScrollBar）；separator_before 的条目在行上方叠一条 1pt outline 分隔线
// （空 Row + 固定高，外层 Column 交叉轴 Stretch 自动拉满菜单宽度），用于
// 分组条目（如"＋ 新建"菜单）；不设该字段的现有菜单渲染零变化。
// **级联子菜单**：item.children 非空 = 父项（行尾 ›），hover/点击以本行为锚
// 向右弹出子层。子层 dismiss_on_outside_press=false → Content 指针策略，不
// 吞父层条目的点击、也不因外部按压自关（同框架 Menu submenu 配方）；同层父
// 项互斥，切换/落到叶子行即关旧子层及其整个子树；根层 outside-press/Esc 经
// on_dismiss_request 递归关子层再关自己（框架见自定义 on_dismiss_request 后
// 不再自动关层，必须自己 Dismiss）。注意：子层锚点在 Show 时刻取行矩形，
// 父层随后滚动不会带动子层（菜单场景可接受）。

// 每菜单层一份级联状态（shared_ptr 随 Show 创建、层销毁释放）。
struct PopupMenuReg {
    // 当前打开的直接子层关闭器（递归关子树并清本指针）；空 = 无子层。
    std::shared_ptr<std::function<void()>> closeChild;
    int childRow = -1; // 子层归属行（hover 重入不重复弹）
};
using PopupMenuRegPtr = std::shared_ptr<PopupMenuReg>;

// 关闭本层当前子层（含其整个子树）。先 move 走再调用：closer 执行中会改写
// reg 字段，避免自销毁调用对象。
void CloseSubMenu(const PopupMenuRegPtr& reg) {
    auto closer = std::move(reg->closeChild);
    reg->childRow = -1;
    if (closer && *closer) (*closer)();
}

// 定义在下文（行与内容互相引用，先挂声明）。
huxerui::View PopupMenuContent(huxerui::PopupContext ctx, std::vector<PopupMenuItem> items,
                               std::vector<huxerui::PopupContext> ancestors, PopupMenuRegPtr reg);

// 菜单行（独立 composable：子层锚点只能挂一个 View，每个父项行需自己的
// UsePopup；稳定 Key 保证 hover 重组不替换节点、锚点关系不断）。
[[huxerui::composable]] huxerui::View PopupMenuRow(huxerui::PopupContext ctx,
                                                   std::vector<huxerui::PopupContext> ancestors,
                                                   PopupMenuRegPtr reg, std::size_t index,
                                                   PopupMenuItem item, huxerui::State<int> hovered,
                                                   huxerui::Color selectedFill) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::MenuStyle menuStyle = huxerui::UseEnvironment<huxerui::MenuStyle>();
    auto popup = huxerui::UsePopup(); // 父项行子层锚点（叶子行闲置）
    const int rowKey = static_cast<int>(index);
    const bool hasChildren = !item.children.empty();
    const bool dangerRed =
        item.danger == PopupMenuDanger::kAlwaysRed ||
        (item.danger == PopupMenuDanger::kHoverRed && hovered.Get() == rowKey);
    const huxerui::Color labelColor =
        item.label_color.has_value() ? *item.label_color
        : dangerRed                  ? theme.colors.error
                                     : menuStyle.foreground;

    // 关闭整条菜单链（同系统菜单）：先关本层已展开子层及子树，再关自身与
    // 各祖先层（ancestors 根在前，逐深到浅关），最后执行条目回调。
    auto dismissChain = [ctx, ancestors, reg] {
        CloseSubMenu(reg);
        ctx.Dismiss();
        for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) it->Dismiss();
    };
    // 弹出子层：同层互斥先关旧子层。closer 存进本层 reg，也挂在子层
    // on_dismiss_request 上——子层被 Esc 自关时走同一关闭路径（清父层指针、
    // 递归关孙层）。id 在 Show 返回后才知，closer 捕获 shared_ptr 延迟读取。
    auto openChild = [popup, ctx, ancestors, reg, rowKey, item] {
        if (reg->childRow == rowKey) return; // hover 重入，子层还在
        CloseSubMenu(reg);
        auto childReg = std::make_shared<PopupMenuReg>();
        std::vector<huxerui::PopupContext> childAncestors = ancestors;
        childAncestors.push_back(ctx);
        auto id = std::make_shared<huxerui::LayerId>(0);
        auto closer = std::make_shared<std::function<void()>>();
        huxerui::PopupOptions options;
        options.placement = {huxerui::AnchorSide::Right, huxerui::AnchorAlignment::Start};
        options.dismiss_on_outside_press = false; // Content 策略，父层保持可交互
        options.on_dismiss_request = [closer] { (*closer)(); };
        *closer = [popup, id, childReg, reg, rowKey] {
            CloseSubMenu(childReg);
            popup.Dismiss(*id);
            if (reg->childRow == rowKey) {
                reg->closeChild = nullptr;
                reg->childRow = -1;
            }
        };
        *id = popup.Show(
            [items = item.children, childAncestors,
             childReg](huxerui::PopupContext childCtx) mutable {
                return PopupMenuContent(childCtx, std::move(items), childAncestors, childReg);
            },
            options);
        reg->childRow = rowKey;
        reg->closeChild = closer;
    };

    huxerui::View row =
        huxerui::Row {
            huxerui::Text(hasChildren ? item.label + "  ›" : item.label, huxerui::TextRole::Body)
                .With(huxerui::Foreground(labelColor)),
        }
            .With(// 选中项：深于 hover 的填充底色（无对钩）。Background 在
                  // Padding 外层，整行铺满。
                  huxerui::Background(item.checked ? selectedFill
                                                   : huxerui::Color::Transparent()),
                  huxerui::CornerRadius(menuStyle.corner_radius / 2.0F),
                  huxerui::Padding(menuStyle.item_padding),
                  huxerui::Frame{.min_height = menuStyle.minimum_item_height},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  // 条目悬停/按压反馈复用菜单样式的 indication（OnClick
                  // 默认 indication 以此覆盖），叠在选中底色之上。
                  menuStyle.item_indication);
    if (hasChildren) row = std::move(row).With(popup.Anchor()); // 子层锚定本行右侧

    // hover：Enter 记悬停行并做子层展开/互斥（父项行打开自己的、叶子行关
    // 兄弟的）；Leave 仅当仍是本行才清悬停态。
    row = std::move(row)
              .On<huxerui::ViewEvents::Hover>(
                  [hovered, reg, rowKey, hasChildren, openChild](const huxerui::HoverEvent& e) {
                      if (e.type == huxerui::HoverEventType::Enter) {
                          hovered = rowKey;
                          if (hasChildren)
                              openChild();
                          else
                              CloseSubMenu(reg);
                      } else if (e.type == huxerui::HoverEventType::Leave &&
                                 hovered.Get() == rowKey)
                          hovered = -1;
                  });
    if (hasChildren && item.on_click) {
        // 父项兼目的地（如"移动到"的子分组）：点击执行回调并关整链，hover 仍可下钻。
        row = std::move(row).OnClick([dismissChain, item] {
            dismissChain();
            if (item.on_click) item.on_click();
        });
    } else if (hasChildren) {
        row = std::move(row).OnClick([openChild] { openChild(); }); // 纯父项：点击兜底展开
    } else {
        row = std::move(row).OnClick([dismissChain, onClick = item.on_click] {
            dismissChain();
            if (onClick) onClick();
        });
    }
    huxerui::View line = std::move(row).Key(static_cast<std::int64_t>(index));
    if (item.separator_before) {
        // 分隔线：空 Row 固定 1pt 高 + outline 底色，外层 Column 交叉轴
        // Stretch 自动拉满菜单宽度；线上/下各留一档间距形成组界。
        line = huxerui::Column {
            huxerui::Row{}.With(huxerui::Frame{.height = 1.0F},
                                huxerui::Background(theme.colors.outline)),
            std::move(line),
        }
            .With(huxerui::Spacing(4.0F),
                  huxerui::Padding(huxerui::EdgeInsets{.top = 4.0F}),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return line;
}

[[huxerui::composable]] huxerui::View PopupMenuContent(huxerui::PopupContext ctx,
                                                       std::vector<PopupMenuItem> items,
                                                       std::vector<huxerui::PopupContext> ancestors,
                                                       PopupMenuRegPtr reg) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::MenuStyle menuStyle = huxerui::UseEnvironment<huxerui::MenuStyle>();
    auto hovered = huxerui::UseState(-1); // 悬停条目下标（Hover 是包含生命周期）
    // 选中底色：比 hover 深一档 = 菜单样式的 press 填充。
    huxerui::Color selectedFill = theme.colors.surface_container_highest;
    if (menuStyle.item_indication.press.has_value() &&
        menuStyle.item_indication.press->fill.has_value()) {
        // 0.2.0 起 Fill 统一为 VisualFill = variant<Brush, ImageFill>，纯色再下钻一层。
        if (const huxerui::Brush* b =
                std::get_if<huxerui::Brush>(&menuStyle.item_indication.press->fill->Get()))
            if (const huxerui::Color* c = std::get_if<huxerui::Color>(&b->Get()))
                selectedFill = *c;
    }
    std::vector<huxerui::View> rows;
    rows.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i)
        rows.push_back(PopupMenuRow(ctx, ancestors, reg, i, items[i], hovered, selectedFill));
    // 条目多时长列表限高滚动（方法下拉有 20 项，不限高会顶穿屏幕）：行列表
    // 进 ScrollView + ScrollBar，max_height 只封顶、内容不足时按内容收缩。
    constexpr float kMenuMaxHeight = 320.0F;
    huxerui::View list = huxerui::ScrollView {
        huxerui::Column{std::move(rows)}.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
    }
                           .With(huxerui::ScrollBar(),
                                 huxerui::Frame{.max_height = kMenuMaxHeight});
    return huxerui::Column{std::move(list)}.With(
        menuStyle.shadow, huxerui::Background(menuStyle.background),
        huxerui::CornerRadius(menuStyle.corner_radius), huxerui::ClipChildren(),
        huxerui::Padding(menuStyle.content_padding),
        huxerui::Frame{.min_width = menuStyle.minimum_width},
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 根层 Show 公共包装：建根 reg；接管线上 on_dismiss_request——outside-
// press/Esc 时先递归关子层再关自己（框架见自定义回调就不再自动关层），并
// 保留调用方原回调。id 延迟绑定：on_dismiss_request 里读 shared_ptr。
namespace {
huxerui::LayerId ShowPopupMenuRoot(huxerui::PopupHandle popup,
                                   std::vector<PopupMenuItem> items,
                                   const huxerui::PopupOptions& options,
                                   const std::optional<huxerui::Point>& at,
                                   std::function<void(bool)> onHover = {}) {
    auto reg = std::make_shared<PopupMenuReg>();
    auto id = std::make_shared<huxerui::LayerId>(0);
    huxerui::PopupOptions opts = options;
    const auto userRequest = opts.on_dismiss_request;
    opts.on_dismiss_request = [popup, id, reg, userRequest] {
        CloseSubMenu(reg);
        popup.Dismiss(*id);
        if (userRequest) userRequest();
    };
    auto factory = [items = std::move(items), reg,
                    onHover = std::move(onHover)](huxerui::PopupContext ctx) mutable {
        huxerui::View content = PopupMenuContent(ctx, std::move(items), {}, reg);
        if (onHover) {
            content = std::move(content).On<huxerui::ViewEvents::Hover>(
                [onHover](const huxerui::HoverEvent& event) {
                    if (event.type == huxerui::HoverEventType::Enter) onHover(true);
                    if (event.type == huxerui::HoverEventType::Leave) onHover(false);
                });
        }
        return content;
    };
    *id = at.has_value() ? popup.ShowAt(*at, std::move(factory), opts)
                         : popup.Show(std::move(factory), opts);
    return *id;
}
} // namespace

void ShowPopupMenu(huxerui::PopupHandle popup, std::vector<PopupMenuItem> items,
                   const huxerui::PopupOptions& options) {
    ShowPopupMenuRoot(std::move(popup), std::move(items), options, std::nullopt);
}

void ShowPopupMenuAt(huxerui::PopupHandle popup, huxerui::Point point,
                     std::vector<PopupMenuItem> items, const huxerui::PopupOptions& options) {
    ShowPopupMenuRoot(std::move(popup), std::move(items), options, point);
}

// P1-B1.2 统一菜单：AppMenuItem → PopupMenuItem 适配（页面不再手写 Popup 布局）
namespace {
PopupMenuItem AppToPopup(const AppMenuItem& a) {
    PopupMenuItem p;
    p.label = a.label;
    p.on_click = a.onClick;
    p.checked = a.checked;
    p.separator_before = a.separatorBefore;
    p.label_color = a.labelColor;
    switch (a.tone) {
        case AppMenuTone::DangerHover:
            p.danger = PopupMenuDanger::kHoverRed;
            break;
        case AppMenuTone::DangerAlways:
            p.danger = PopupMenuDanger::kAlwaysRed;
            break;
        default:
            p.danger = PopupMenuDanger::kNone;
            break;
    }
    // 快捷键拼到 label 右侧（Popup 暂无独立 shortcut 列）
    if (a.shortcut && !a.shortcut->empty()) {
        p.label += "  " + *a.shortcut;
    }
    // icon 暂透传：Popup 阶段不渲染图标，保留字段供后续菜单视觉统一时启用
    for (const AppMenuItem& c : a.children) {
        p.children.push_back(AppToPopup(c));
    }
    // enabled=false 时去活：on_click 置空，label 用 disabled 色（由调用方通过 labelColor 或 tone 表达亦可）
    if (!a.enabled) {
        p.on_click = nullptr;
        if (!p.label_color) {
            // 用 on_surface_variant 的半透明近似 disabled（由主题派生时更准，这里用固定 0.4 透明）
            // 调用方可显式传 labelColor 覆盖
        }
        // 去活的子树亦去活
        for (PopupMenuItem& child : p.children) child.on_click = nullptr;
    }
    return p;
}
} // namespace

void ShowAppMenu(huxerui::PopupHandle popup, std::vector<AppMenuItem> items,
                 const huxerui::PopupOptions& options) {
    std::vector<PopupMenuItem> popupItems;
    popupItems.reserve(items.size());
    for (const AppMenuItem& a : items) popupItems.push_back(AppToPopup(a));
    ShowPopupMenu(std::move(popup), std::move(popupItems), options);
}
void ShowAppMenuAt(huxerui::PopupHandle popup, huxerui::Point point, std::vector<AppMenuItem> items,
                   const huxerui::PopupOptions& options) {
    std::vector<PopupMenuItem> popupItems;
    popupItems.reserve(items.size());
    for (const AppMenuItem& a : items) popupItems.push_back(AppToPopup(a));
    ShowPopupMenuAt(std::move(popup), point, std::move(popupItems), options);
}

huxerui::LayerId ShowHoverAppMenu(huxerui::PopupHandle popup,
                                  std::vector<AppMenuItem> items,
                                  std::function<void(bool)> onMenuHover,
                                  const huxerui::PopupOptions& options) {
    std::vector<PopupMenuItem> popupItems;
    popupItems.reserve(items.size());
    for (const AppMenuItem& item : items) popupItems.push_back(AppToPopup(item));
    return ShowPopupMenuRoot(std::move(popup), std::move(popupItems), options, std::nullopt,
                             std::move(onMenuHover));
}

// 方法 + URL 合并控件（Postman 风格）：左侧扁平方法选择（统一无尾下箭头弹自绘下拉，
// DELETE 常驻红；保持自绘而非官方 Select——Select 触发器自带描边外观，塞不进
// 这个共用外框的组合栏），其后是当前环境 baseUrl 显示区（灰色只读、截断；
// 输入框内容带 URI scheme 时以输入为准不拼接 → 该段半透明弱化），中间 1pt
// 分隔线，右侧 URL 输入；整体共用一个描边圆角外框。外框圆角 = 几何令牌
// large_control_radius（12pt）：请求 URL 行是高密度工具条，用 10–12pt 大圆角
// 组合栏、不做 full capsule（§5.2）。方法触发器/baseUrl 段/分隔线/URL 输入共享
// 同一外轮廓，内部不重复描边——URL 字段经 ProvideEnvironment 局部覆盖
// TextFieldStyle：透明描边 + 零圆角，边框完全交给外框（TextField 无单实例样式
// API，Environment 是最窄机制）；零圆角即与外框同心的极限（内层不再有独立
// 底/角，无需 ConcentricRadius）。
[[huxerui::composable]] huxerui::View MethodUrlBar(
    std::vector<std::string> methods, std::size_t methodIndex,
    std::function<void(std::size_t)> onMethodChanged, huxerui::TextEditingValue url,
    std::function<void(const huxerui::TextEditingValue&)> onUrlChanged,
    std::string baseUrl, std::string placeholder) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto popup = huxerui::UsePopup(); // 方法下拉：自绘内容（DELETE 常驻红，MenuItem 无文字配色 API）
    const std::size_t safe = methodIndex < methods.size() ? methodIndex : 0;

    huxerui::TextFieldStyle urlStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    urlStyle.outlined.border = huxerui::Color::Transparent();
    urlStyle.outlined.hovered_border = huxerui::Color::Transparent();
    urlStyle.outlined.focused_border = huxerui::Color::Transparent();
    urlStyle.corner_radius = 0.0F;
    // 高度对齐旁边的"发送"按钮：ButtonStyle 默认 minimum_height=0，按钮高由内容
    // 撑出 = 14pt 文字行高（约 16pt）+ padding Symmetric(14,8) 的 16pt ≈ 32pt；
    // 而 Outlined 变体默认 minimum_height=36，整条控件比按钮高一截。这里把
    // minimum_height 收到 control_height(32)，内容高（14pt 文字 + 垂直 padding
    // 12pt ≈ 28pt）低于它，由 minimum_height 定高且文字垂直居中；padding 同步
    // 收 8→6 保持居中余量。
    urlStyle.outlined.minimum_height = islands.control_height;
    urlStyle.padding = huxerui::EdgeInsets::Symmetric(10.0F, 6.0F);

    // 方法触发器：扁平文本，点击弹锚定 Popup（自绘下拉，外观对齐菜单层语义）。
    // 垂直内边距 8→6：与上面 URL 字段收到的 32pt 同高对齐（文字 14pt + 12pt ≈
    // 26pt < 32pt，外层 Row Stretch 拉满后由 CrossAlign(Center) 垂直居中）。
    // 触发器与下拉项文字都按 MethodColor 统一色表逐方法着色。
    huxerui::View trigger =
        huxerui::Row {
            huxerui::Text(methods.at(safe), huxerui::TextRole::Body)
                .With(huxerui::Foreground(MethodColor(theme, methods.at(safe)))),
            huxerui::Image(app::images::chevron_down)
                .Fit(huxerui::ImageFit::Contain)
                .Align(huxerui::HorizontalAlignment::Center,
                       huxerui::VerticalAlignment::Center)
                .Tint(theme.colors.on_surface_variant)
                .With(huxerui::Frame{.width = 12.0F, .height = 12.0F}),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                  huxerui::Spacing(theme.spacing.extra_small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .OnClick([popup, methods = std::move(methods), current = safe,
                      onChanged = std::move(onMethodChanged), &theme] {
                // 点击时组件仍挂载、theme 引用有效；颜色在弹层内容里随点击
                // 时刻烘焙（菜单打开期间切主题属极端情况，重开即刷新）。
                std::vector<PopupMenuItem> items;
                items.reserve(methods.size());
                for (std::size_t i = 0; i < methods.size(); ++i) {
                    items.push_back(PopupMenuItem{
                        .label = methods[i],
                        .on_click = [onChanged, i] { onChanged(i); },
                        .checked = i == current,
                        .label_color = MethodColor(theme, methods[i]),
                    });
                }
                ShowPopupMenu(popup, std::move(items),
                              huxerui::PopupOptions{
                                  .placement = {huxerui::AnchorSide::Below,
                                                huxerui::AnchorAlignment::Start}});
            });
    trigger = std::move(trigger).With(popup.Anchor());

    // baseUrl 显示区：当前环境基础 URL（灰色只读、max_width 截断）。输入框自带
    // URI scheme 时以输入为准（finalizeSpec 不拼接）→ 该段半透明弱化提示；
    // baseUrl 为空（无环境/未配置）时整段不渲染。
    huxerui::View baseSegment = huxerui::Row{};
    if (!baseUrl.empty()) {
        const bool overridden = hasUriScheme(trim(url.text));
        baseSegment = huxerui::Row {
            huxerui::Text(baseUrl, huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 0.0F)),
                  huxerui::Frame{.max_width = 160.0F}, huxerui::ClipChildren(),
                  // 外层 Row 是 Stretch（为分隔线拉满全高），本段需自行垂直居中。
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Opacity(overridden ? 0.4F : 1.0F));
    }

    return huxerui::Row {
        std::move(trigger),
        std::move(baseSegment),
        // 分隔线：父 Row 交叉轴 Stretch 拉满全高。
        huxerui::Column{}.With(huxerui::Frame{.width = 1.0F},
                               huxerui::Background(theme.colors.outline)),
        huxerui::ProvideEnvironment(
            urlStyle,
            huxerui::View{huxerui::TextField(url)
                              .Placeholder(std::move(placeholder))
                              .Variant(huxerui::TextFieldVariant::Outlined)
                              .OnChanged(std::move(onUrlChanged))
                              .With(huxerui::Grow(1.0F))}),
    }
        .With(huxerui::Spacing(0.0F),
              huxerui::Border(theme.colors.outline, 1.0F),
              huxerui::CornerRadius(islands.large_control_radius), huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

// 页面标题 + 副标题。
[[huxerui::composable]] huxerui::View PageHeader(std::string title, std::string subtitle) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
               huxerui::Text(std::move(title), huxerui::TextRole::Title),
               huxerui::Text(std::move(subtitle), huxerui::TextRole::Body),
           }
        .With(huxerui::Spacing(4.0F), huxerui::Foreground(theme.colors.on_surface_variant));
}

// 尚未迁移完成的页面占位。
[[huxerui::composable]] huxerui::View MigrationPlaceholder(std::string pageName) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
               huxerui::Text(std::move(pageName) + " — 迁移中", huxerui::TextRole::Title),
               huxerui::Text("此页面正在从 EUI-NEO 前端迁移到 HuxerUI，功能暂不可用。",
                             huxerui::TextRole::Body),
           }
        .With(huxerui::Padding(theme.spacing.large), huxerui::Spacing(8.0F),
              huxerui::Foreground(theme.colors.on_surface_variant));
}

} // namespace apitab::ui
