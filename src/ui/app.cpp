// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘）：
//   标题栏岛：Logo(AT) + 顶级标签条（TopTabStrip：主页钉在最左、项目标签横向滚动、
//     设置单例标签固定追加在所有项目标签之后）+ 闪电（遗留 HttpTest 入口）+ 齿轮
//     (全局设置单例标签) + 框架窗口按钮；收窄为 24px 高、去背景直接融入窗口底色，
//     主题为极简 AI 黑白风（MinimalDark/MinimalLightThemeSpec，对齐 tinynext 配色）。
//   下方：左侧图标侧边栏（无岛屿包裹，直接落在窗口背景上）｜内容区（页面自己的
//   一级岛屿划分区域，外壳不再套岛）。根节点刷整窗底色（rootSpec.colors.background——
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
// 一并清理；kHttpTest 为遗留全宽顶级入口（见 AppRoot 内容区与闪电按钮注释）。
enum PageIndex : std::size_t {
    kRequest = 0,
    kLoad = 1,
    kHistory = 2,
    kProjectSettings = 3,
    kHttpTest = 4, // 遗留：框架 HTTP 协程压测实验页（标题栏闪电图标进入，全宽显示）
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
        // kHttpTest 不走本函数：它是遗留的全宽顶级入口，由 AppRoot 内容区直接渲染
        //（§13.1：P1-B1 项 4 评估其顶级身份），不属于项目工作区页面。
        case kRequest:
        default:
            // 主页已整宽覆盖侧栏：未打开项目时本页不可达，无需兜底。
            return RequestPage(activeProject);
    }
}
} // namespace pages

namespace {

// 标题栏内容统一高度：Logo / 主页标签 / 项目标签 / 齿轮全部 20pt，垂直居中。
// 20pt 低于系统窗口按钮条的最小高度，不再由我们的内容把整条标题栏顶高。
// 标题栏内容统一高度：与 AppOptions.window.title_bar_height（24）一致——标签、
// Logo、齿轮与标题栏等高撑满，文字行高（System 12 ≈ 16pt）不再被 20pt 框裁掉。
constexpr float kTitleBarContentHeight = 24.0F;
// 项目标签固定宽度：拖拽换位/边缘钳制需要已知步进（宽 + 间距），同 Chrome
// 固定宽标签。主页图标标签不在此列（32pt 见 TopTab）；设置单例标签与其同宽。
constexpr float kProjectTabWidth = 140.0F;
// 设置单例标签的条内显示 key（.Key/hoveredTab/dragId 的条内比较用）：仅作显示
// key，绝不进入 open_projects/持久化数据；领域/状态模型里设置由
// TopTabKind::GlobalSettings 表达（§13.4 B0.1：不允许用负数 project id 充当设置）。
constexpr std::int64_t kSettingsTabDisplayKey = std::numeric_limits<std::int64_t>::max();

// 条内显示 key：主页 0（沿用旧「activeProject==0 = 主页」的条内约定）、项目 =
// 项目 id、设置 = kSettingsTabDisplayKey。
std::int64_t TopTabDisplayKey(TopTabId tab) {
    switch (tab.kind) {
        case TopTabKind::Project:
            return tab.project_id;
        case TopTabKind::GlobalSettings:
            return kSettingsTabDisplayKey;
        case TopTabKind::Home:
            break;
    }
    return 0;
}

// 极简 AI 黑白风主题（对齐 tinynext 的 heibu geekBlack/geekWhite 配色）：
// 深色 = 近纯黑底 + 纯白主色（主色控件白底黑字）；浅色 = 近白底 + 纯黑主色。
// 文本/描边只用地道中灰，状态色仅 error 保留柔和红。
huxerui::ThemeSpec MinimalDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.colors.primary = huxerui::Color::Rgb(255, 255, 255);      // 纯白主色
    spec.colors.on_primary = huxerui::Color::Rgb(10, 10, 12);      // 白底上翻黑
    spec.colors.secondary = huxerui::Color::Rgb(214, 214, 217);
    spec.colors.on_secondary = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.secondary_container = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.background = huxerui::Color::Rgb(10, 10, 12);      // #0A0A0C 近纯黑
    spec.colors.surface = huxerui::Color::Rgb(14, 14, 17);
    spec.colors.surface_container_low = huxerui::Color::Rgb(19, 19, 22);
    spec.colors.surface_container = huxerui::Color::Rgb(24, 24, 28);
    spec.colors.surface_container_high = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(37, 37, 43);
    spec.colors.on_surface = huxerui::Color::Rgb(242, 242, 242);   // 0.95 白
    spec.colors.on_surface_variant = huxerui::Color::Rgb(148, 148, 153); // 0.58 灰
    spec.colors.outline = huxerui::Color::Rgb(46, 46, 52);
    spec.colors.inverse_surface = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.error = huxerui::Color::Rgb(235, 122, 112);        // tinynext 柔和红
    return spec;
}

huxerui::ThemeSpec MinimalLightThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialLightThemeSpec();
    // 冷中性灰白：保留柔和层级，但去掉上一版米白中过强的黄/棕分量。
    spec.colors.primary = huxerui::Color::Rgb(37, 40, 45);         // #25282D
    spec.colors.on_primary = huxerui::Color::Rgb(250, 250, 251);   // #FAFAFB
    spec.colors.secondary = huxerui::Color::Rgb(104, 112, 124);    // #68707C
    spec.colors.on_secondary = huxerui::Color::Rgb(250, 250, 251);
    spec.colors.secondary_container = huxerui::Color::Rgb(231, 234, 240);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(37, 40, 45);
    spec.colors.background = huxerui::Color::Rgb(243, 244, 246);   // #F3F4F6 海面
    spec.colors.surface = huxerui::Color::Rgb(250, 250, 251);      // #FAFAFB
    spec.colors.surface_container_low = huxerui::Color::Rgb(248, 249, 250);
    spec.colors.surface_container = huxerui::Color::Rgb(241, 243, 245);
    spec.colors.surface_container_high = huxerui::Color::Rgb(231, 234, 238);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.on_surface = huxerui::Color::Rgb(36, 39, 44);      // #24272C
    spec.colors.on_surface_variant = huxerui::Color::Rgb(107, 114, 128); // #6B7280
    spec.colors.outline = huxerui::Color::Rgb(216, 220, 226);      // #D8DCE2
    spec.colors.inverse_surface = huxerui::Color::Rgb(36, 39, 44);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(250, 250, 251);
    spec.colors.error = huxerui::Color::Rgb(204, 64, 51);
    return spec;
}

// 主题边界：MaterialThemeDefinition(spec) 之上用 typed style 覆盖组件样式——
// 长按钮（Button）与分段按钮（SegmentedButton）的圆角统一 8px
// （M3 默认是全圆胶囊）。Default() 静态基线角半径本来就是 8，重新着色到我们的
// 黑白调色板即可（DefaultXxxStyle(spec) 在 detail 命名空间，非公共契约，不用）。
huxerui::View MinimalThemed(bool dark, huxerui::View content) {
    const huxerui::ThemeSpec spec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    huxerui::ThemeDefinition definition = huxerui::MaterialThemeDefinition(spec);

    huxerui::ButtonStyle buttons; // Default()：corner_radius=8、padding Symmetric(14,8)
    buttons.background = spec.colors.primary;
    buttons.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                             spec.colors.on_primary};
    definition.Set(buttons);

    huxerui::SegmentedButtonStyle segments; // Default()：corner_radius=8
    segments.background = spec.colors.surface;
    segments.selected_background = spec.colors.primary;
    segments.label_style = huxerui::TextStyle{huxerui::Font::System(font_size::kBody),
                                              spec.colors.on_surface};
    segments.selected_label = spec.colors.on_primary;
    segments.border = spec.colors.outline;
    segments.selected_border = spec.colors.primary;
    definition.Set(segments);

    // 内置确认框（DialogHandle::Show(title, message, ...) 形态）跟随主题：DialogStyle
    // 是 Environment 值（presentation.h 有 Default()/==），经 ThemeDefinition::Set
    // 全局覆盖。Default() 基线是白底浅色配色，逐字段换色。
    // 删除/清空等破坏性确认已统一走 ui 层 ShowDangerConfirm（确认按钮染 error 红），
    // 这里的覆盖保留为内置形态的兜底主题。
    // 叠加层（hover/press）改用 on_surface/on_primary 派生的半透明色，
    // 否则深色下黑叠黑、浅色主色（黑底）上白叠加不可见。
    const auto withAlpha = [](huxerui::Color c, float a) {
        c.alpha = a;
        return c;
    };
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
    definition.Set(dialogs);

    // 下拉选择（Select，全局设置页/历史页在用）跟随主题：颜色对齐 Material
    // 派生（detail::MaterialSelectStyle 非公共契约，逐字段铺开），触发框与
    // 弹出菜单圆角统一 8px（与 Button/SegmentedButton 一致；M3 默认是 4/8）。
    // 叠加层沿用 DialogStyle 的 on_surface 半透明做法，不用 M3 ripple。
    huxerui::SelectStyle selects;
    selects.background = spec.colors.surface_container_highest;
    selects.foreground = spec.colors.on_surface;
    selects.border = spec.colors.outline;
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
    selects.popup_shadow = huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 8.0F, 0.0F};
    selects.content_spacing = spec.spacing.small;
    selects.validation_spacing = spec.spacing.extra_small;
    selects.minimum_height = 48.0F;
    selects.minimum_item_height = 40.0F;
    selects.indicator_size = 20.0F;
    selects.corner_radius = spec.shapes.small;
    selects.popup_corner_radius = spec.shapes.small;
    const huxerui::Indication selectIndication{
        .hover = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.08F)},
        .press = huxerui::IndicationLayer{.fill = withAlpha(spec.colors.on_surface, 0.12F)},
    };
    selects.indication = selectIndication;
    selects.item_indication = selectIndication;
    definition.Set(selects);

    return huxerui::Theme(std::move(definition), content);
}

// 软件徽标：两个圆角岛由请求路径连接的 apitab 标记，不再使用字母 AT。
// SVG 保持单色并由 Tint(on_primary) 适配主题；外层色块沿用主题 primary，保证
// 24pt 标题栏和深浅主题下都有稳定对比度。
[[huxerui::composable]] huxerui::View LogoBadge() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Image(app::images::apitab_mark)
        .Fit(huxerui::ImageFit::Contain)
        .Align(huxerui::HorizontalAlignment::Center,
               huxerui::VerticalAlignment::Center)
        .Tint(theme.colors.on_primary)
        .With(huxerui::Frame{.width = 32.0F, .height = kTitleBarContentHeight},
              huxerui::Background(theme.colors.primary),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::Padding(4.0F));
}

// 顶级标签拖拽载荷：按项目 id 定位源/目标标签。只接受项目——主页标签（固定最左）
// 与设置单例标签（不参与排序、不写入 open_projects，§13.4 B0.1）不挂 DragSource。
struct ProjectTabDragPayload {
    std::int64_t projectId = 0;
};

// 顶级标签条 → AppRoot 的操作入口：activate/close 的实现由 AppRoot 提供（推迟任务
// 里完成 TopTabState helper 计算、领域写入与 State 写回，TopTab/TopTabStrip 不直接
// 持有顶级状态写入）。
struct TopTabActions {
    std::function<void(TopTabId)> activate;
    std::function<void(TopTabId)> close;
};

// 单个顶级标签：激活态 = 最高层级容器底 + 主文字色；未激活 = 略深容器底 + 次级文字色。
// 整块外层只负责激活与切换（点击会卸载内容子树，切换统一经 actions.activate 的
// AppRoot 推迟任务执行，CLAUDE.md 约定 6）；内层用两个兄弟节点分别承载「切换」与
// 「关闭」，避免各自做一次整标签的背景重绘。主页标签（kind=Home）不可关闭、不挂
// 拖拽，位置恒定最左；项目标签（kind=Project）可关闭、挂拖拽换位；设置单例标签
// （kind=GlobalSettings）可关闭、不挂拖拽——拖拽 payload 只接受项目，设置不参与
// 拖拽排序、不写入 open_projects（§13.4 B0.1）。拖拽的 strip 级状态（dragId/dragDx/
// dragOrig）与几何（index/count/stride）由 TopTabStrip 传入：拖动时本标签变透明
// 占位，视觉由条内覆盖层克隆接管。
[[huxerui::composable]] huxerui::View TopTab(
    TopTabId tab, std::string name, huxerui::View leading, bool active, bool draggable,
    const TopTabActions& actions, huxerui::State<std::vector<std::int64_t>> tabs,
    huxerui::State<std::int64_t> dragId, huxerui::State<float> dragDx,
    huxerui::State<int> dragOrig, std::size_t index, std::size_t count, float stride,
    huxerui::State<std::shared_ptr<SlideCell>> slideCell,
    huxerui::State<std::uint64_t> slideTick,
    huxerui::State<std::int64_t> hoveredTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    // 条内显示 key（TopTabDisplayKey，见上）：悬停/hoveredTab 与分隔竖线显隐用。
    const std::int64_t key = TopTabDisplayKey(tab);
    // 悬停才显示 ✕ 与背景。官方 view 级 Hover 事件（containment 生命周期）：
    // 指针进入标签呈现边界 Enter、真正离开才 Leave，在子组件（切换区/✕）之间
    // 移动不触发 Leave，天然等价于原来三个 HoverTrack 共享 cell 的聚合语义。
    // 同步写 strip 级 hoveredTab（分隔竖线显隐要用；-1 = 无）。
    auto hovered = huxerui::UseState(false);
    // P1-B0.5 键盘关闭：关闭按钮始终 enabled/focusable，hover∪focus 控制可见。
    auto closeFocused = huxerui::UseState(false);
    // 拖动中（仅本标签被拖时）变透明占位：保留布局槽位与拖拽会话，视觉由
    // TopTabStrip 的覆盖层克隆接管——覆盖层无任何事件 handler，命中测试
    // 穿透到下方静止标签。dragId 只会持有项目 id（主页/设置标签不挂 DragSource）。
    const bool dragging = draggable && dragId.Get() == tab.project_id;
    // 可关闭：主页标签固定不可关；项目标签与设置单例标签可关。
    const bool closable = tab.kind != TopTabKind::Home;

    // 标题栏已无底色（融入窗口背景）；标签背景默认也不显示（透明）——
    // 激活 = surface_container_highest 提亮，悬停 = surface_container 浮起，
    // 常态下只靠标签间的竖线分隔。
    const huxerui::Color tabFill =
        active ? theme.colors.surface_container_highest
               : (hovered.Get() ? theme.colors.surface_container
                                : huxerui::Color::Transparent());
    const huxerui::Color tabForeground =
        active ? theme.colors.on_surface : theme.colors.on_surface_variant;

    // 激活/关闭统一走 AppRoot 注入的 actions（推迟任务里完成顶级标签状态写回与
    // 领域同步）；本项目不再直接持有任何顶级状态写入。
    const auto activate = [actions, tab] { actions.activate(tab); };

    const auto badgeFont =
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::SemiBold);
    // 主页标签只放图标：省略文字、收窄边距并固定窄宽，避免挤占项目标签空间；
    // 限宽与裁剪只压内部「切换区」（图标+文字），长项目名截断而行尾 ✕ 永远完整显示。
    // 所有标签统一 kTitleBarContentHeight 高；内外两层 Row 都交叉轴居中，
    // 图标/文字/✕ 不会在 24pt 条里各自顶格漂移。
    const bool iconOnly = name.empty();
    // 键盘/语义（P1-B0.4，§13.6 键盘要求）：切换区可聚焦 + Button 语义，Tab 到
    // 标签后 Enter/Space 激活 = 切换顶级标签（普通 Row 默认不可聚焦，仅内置
    // Button/Chip 等自带 focusable，模式同 settings_page 左分类行）。主页标签
    // 无文字，语义名固定"主页"；项目/设置标签用条内文字。
    const std::string switchLabel = iconOnly ? std::string{"主页"} : name;
    huxerui::View tabView = huxerui::Row {
        // 切换区：点击 = 激活本标签。max_width 给行尾 ✕ 留出位置
        // （固定宽 140 内：切换区 ≤100 + 28pt ✕ 命中区 + 间隙）。
        huxerui::Row {std::move(leading),
                      iconOnly
                          ? huxerui::View{huxerui::Row{}}
                          : huxerui::View{huxerui::Text(name, huxerui::TextRole::Label)
                                              .Style(huxerui::TextStyle{
                                                  .font = badgeFont,
                                                  .foreground = tabForeground})}}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                  huxerui::Spacing(4.0F),
                  huxerui::Frame{.max_width = kProjectTabWidth - 40.0F},
                  huxerui::ClipChildren(),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  // 整标签的悬停/选中反馈由外层 tabFill 承担；显式空 Indication
                  // 压掉 OnClick 的默认高亮，避免名称区再叠一层。
                  huxerui::Indication{},
                  huxerui::Focusable(true),
                  huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                     .label = switchLabel})
            .OnClick(activate),
        // 弹性占位把 ✕ 顶到固定宽标签的右缘（Spacer 自带 Grow(1)）。
        closable ? huxerui::View{huxerui::Spacer{}} : huxerui::View{huxerui::Row{}},
        // 关闭区（可关标签）：常驻、透明占位，悬停（本标签任一部分）才显示。
        // ✕ 已从 Text+OnClick 迁为 Bare AppIconButton（§13.4 B0.3 中归 B0.1 shell
        // agent 的部分）：固定 28pt 命中区在 24pt 标题栏里上下各溢出 2pt——允许
        // （Bare hover 底轻微出血可接受），不回退成文本按钮。Opacity 只改绘制不动
        // 结构，避免悬停重组换子节点类型引起抖动；enabled=hovered 门控透明占位的
        // 点击（关闭会卸载本标签，AppRoot 侧再经推迟任务执行，约定 6）。
        // 键盘缺口（P1-B0.4 如实记录）：enabled=hovered 使未悬停的 ✕ 为 disabled，
        // disabled 节点不参与 Tab 遍历（runtime.cpp CollectFocusableNodes 要求
        // enabled && focusable）→ 键盘无法到达 ✕，关设置/项目标签暂只能鼠标完成
        //（§13.6 键盘要求中唯一缺口；改门控会变更指针行为，留 P1-B1 顶部导航岛
        // 收束时统一决策，如 hover∪focus 门控或键盘快捷键）。
        closable
            ? huxerui::View{AppIconButton(
                                  "✕",
                                  tab.kind == TopTabKind::GlobalSettings ? "关闭设置标签"
                                                                         : "关闭项目标签",
                                  [actions, tab] { actions.close(tab); },
                                  AppIconButtonShape::Bare, 28.0F, false, true)
                                  .With(huxerui::Opacity((hovered.Get() || closeFocused.Get()) ? 1.0F
                                                                                               : 0.0F))
                                  .On<huxerui::ViewEvents::FocusChanged>(
                                      [closeFocused](bool focused) { closeFocused = focused; })}
            : huxerui::View{huxerui::Row{}},
    }
        .With(huxerui::Spacing(0.0F), huxerui::Background(tabFill),
              huxerui::Foreground(tabForeground),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Padding(iconOnly ? huxerui::EdgeInsets::Symmetric(4.0F, 1.0F)
                                        : huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
              iconOnly ? huxerui::Frame{.width = 32.0F, .height = kTitleBarContentHeight}
                       : huxerui::Frame{.width = kProjectTabWidth,
                                        .height = kTitleBarContentHeight},
              // 拖动时本体透明占位（布局槽位不变），视觉走覆盖层克隆。
              huxerui::Opacity(dragging ? 0.0F : 1.0F),
              // 让位滑动残量（非换位标签恒 0）：实时换位时邻居从旧槽位滑入。
              huxerui::Offset(huxerui::Point{SlideOffsetOf(slideCell.Get(), key), 0.0F}),
              // 外层压掉默认 Indication：热区兜底点击（见下）不再叠一层按压高亮。
              huxerui::Indication{})
        // 整标签热区兜底：点击不冒泡（最深绑定生效），切换区（图标+文字）与 ✕
        // 之间的 Spacer/边距没有任何子绑定，点这里原本无响应——外层挂
        // activate，只会在无更深绑定的空白处命中。
        .OnClick(activate)
        // 悬停（containment）：进入边界置位，真正离开才清除；Leave 仅当
        // hoveredTab 仍是本标签才清 -1，避免竞态清掉邻居刚写入的 Enter。
        .On<huxerui::ViewEvents::Hover>(
            [hovered, hoveredTab, key](const huxerui::HoverEvent& e) {
                if (e.type == huxerui::HoverEventType::Enter) {
                    hovered = true;
                    hoveredTab = key;
                } else if (e.type == huxerui::HoverEventType::Leave) {
                    hovered = false;
                    if (hoveredTab.Get() == key) hoveredTab = -1;
                }
            });

    // 项目标签拖拽换位（主页/设置标签不挂任何拖拽修饰符：主页位置恒定最左，
    // 设置不参与排序且不写入 open_projects，§13.4 B0.1）。
    // 限水平轴（axis=Horizontal）：标签只在标签条行内移动，竖向拖出不进拖拽。
    // Chrome 式贴条滑动：无悬浮拖影；Changed 回写钳制后的 X 位移（范围 =
    // 本标签到条内容两端，拖到容器外贴边停住而不是被裁掉），驱动
    // TopTabStrip 的覆盖层克隆。重排后持久化 open_projects。
    // 注：设计上预留「竖向拖出 = 拆成独立窗口、可拖回」，但 SDK 0.2.0 仍是单窗口
    // 模型（UseWindow 只有主窗口命令，无新建窗口 API；拖放是单视图树内
    // in-process），待 SDK 支持多窗口后把这里改回二维拖拽并加拆出逻辑。
    // 完整方案与检索规则见 docs/plans/project-tab-tear-off-window.md。
    if (draggable) {
        tabView = std::move(tabView)
                      .With(huxerui::DragSource(
                          ProjectTabDragPayload{tab.project_id},
                          huxerui::DragGesture{.axis = huxerui::Axis::Horizontal}))
                      // 拖拽开始：记录被拖标签与起点槽位。
                      .On<huxerui::DragSourceEvents::Started>(
                          [dragId, dragOrig, id = tab.project_id,
                           index](const huxerui::DragEvent&) {
                              dragId = id;
                              dragOrig = static_cast<int>(index);
                          })
                      // 拖动中每帧：钳制后的累计 X 位移写入 dragDx（驱动覆盖层，
                      // axis=Horizontal 时 translation 已被约束到 X），并按"经过
                      // 即换位"实时移动 tabs 顺序——目标槽位 = 起点 +
                      // round(位移/步进)；每次换位顺带持久化 open_projects。
                      // 钳制范围 = 起点槽位到条内容两端，拖到容器外贴边停住。
                      // keyed 重排不卸载节点，同步写即可。
                      .On<huxerui::DragSourceEvents::Changed>(
                          [dragId, dragDx, dragOrig, tabs, tasks, slideCell, slideTick,
                           id = tab.project_id, count,
                           stride](const huxerui::DragEvent& e) {
                              dragId = id;
                              const float orig = static_cast<float>(dragOrig.Get());
                              const float hi =
                                  static_cast<float>(count > 0 ? count - 1 : 0) * stride -
                                  orig * stride;
                              const float t = std::clamp(e.translation.x, -orig * stride, hi);
                              dragDx = t;
                              long desired =
                                  static_cast<long>(orig) + std::lround(t / stride);
                              desired = std::clamp<long>(
                                  desired, 0, static_cast<long>(count > 0 ? count - 1 : 0));
                              std::vector<std::int64_t> copy = tabs.Get();
                              const auto fromIt = std::ranges::find(copy, id);
                              if (fromIt == copy.end()) return;
                              const auto from = static_cast<std::size_t>(
                                  std::distance(copy.begin(), fromIt));
                              const auto target = static_cast<std::size_t>(desired);
                              if (from == target || target >= copy.size()) return;
                              // 邻居让位滑动：from<target 时 (from,target] 左移一格
                              // （残量 +stride），反之 [target,from) 右移（-stride）。
                              if (from < target) {
                                  for (std::size_t k = from + 1; k <= target; ++k)
                                      StartSlide(tasks, slideCell, slideTick, copy[k], stride);
                              } else {
                                  for (std::size_t k = target; k < from; ++k)
                                      StartSlide(tasks, slideCell, slideTick, copy[k], -stride);
                              }
                              const std::int64_t moved = *fromIt;
                              copy.erase(fromIt);
                              copy.insert(copy.begin() + static_cast<long>(target), moved);
                              tabs = copy;
                              std::string csv;
                              for (std::size_t i = 0; i < copy.size(); ++i) {
                                  csv += (i ? "," : "");
                                  csv += std::to_string(copy[i]);
                              }
                              saveSessionPreference("open_projects", csv);
                          })
                      // 结束/取消：归零会移除覆盖层节点（卸载），推迟出指针事件路径。
                      .On<huxerui::DragSourceEvents::Ended>(
                          [tasks, dragId, dragDx](const huxerui::DragDropResult&) {
                              tasks.Launch([=]() -> huxerui::Task<void> {
                                  co_await huxerui::Delay(std::chrono::duration<double>{0});
                                  dragId = 0;
                                  dragDx = 0.0F;
                              });
                          })
                      .On<huxerui::DragSourceEvents::Canceled>(
                          [tasks, dragId, dragDx](const huxerui::DragEvent&) {
                              tasks.Launch([=]() -> huxerui::Task<void> {
                                  co_await huxerui::Delay(std::chrono::duration<double>{0});
                                  dragId = 0;
                                  dragDx = 0.0F;
                              });
                          });
    }
    return tabView;
}

// 顶级标签条（P1-B0.1，由 ProjectTabStrip 泛化为通用顶级标签条）：主页标签
// （房子图标，固定不可关）钉在最左不参与滚动；其余每个打开的项目一个可关、可拖
// 标签，放进横向 ScrollView，溢出时滚动而不是挤变形；设置单例标签（打开时）固定
// 追加在所有项目标签之后——布局决定：主页恒最左、设置恒队尾，两者都不参与项目
// 拖拽排序与 open_projects 持久化（§13.1）。
[[huxerui::composable]] huxerui::View TopTabStrip(
    huxerui::State<std::vector<std::int64_t>> tabs, huxerui::State<bool> settingsOpen,
    huxerui::State<TopTabId> activeTopTab, const TopTabActions& actions) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 拖拽中的 strip 级状态：被拖标签显示 key + 钳制后的累计 X 位移 + 起点槽位
    // （见 TopTab）。dragId 只会持有项目 id（主页/设置标签不挂 DragSource），
    // kSettingsTabDisplayKey 仅作显示 key、绝不进入 open_projects/持久化数据。
    // 拖动时被拖标签本体透明占位，视觉由下方覆盖层克隆接管（Stack 最后声明 =
    // 绘制最上层；克隆无事件 handler，命中测试穿透到下方静止标签）。
    auto dragId = huxerui::UseState<std::int64_t>(0);
    auto dragDx = huxerui::UseState(0.0F);
    // 拖动起点槽位（Started 时记录）：覆盖层 X = 起点槽位 + 累计位移，
    // 与实时换位后的数据下标解耦，视觉连续不跳变。
    auto dragOrig = huxerui::UseState(0);
    // 让位滑动（StartSlide/SlideCell 见 ui.h）：tick 仅作重组触发器。
    auto slideCell = huxerui::UseState(std::make_shared<SlideCell>());
    auto slideTick = huxerui::UseState<std::uint64_t>(0);
    (void)slideTick.Get(); // 订阅：tween 每步 bump 触发重组
    // 悬停标签显示 key（-1 = 无；主页 0、设置 = kSettingsTabDisplayKey 也会写入）：
    // 分隔竖线显隐用。
    auto hoveredTab = huxerui::UseState<std::int64_t>(-1);
    // 步进 = 标签宽 + 分隔竖线(1pt) + 两侧间距（竖线作为 Row 子节点占布局，
    // 用 Opacity 显隐避免悬停时回流抖动）。
    const float tabStride = kProjectTabWidth + 1.0F + 2.0F * theme.spacing.small;
    // 标签间分隔竖线：相邻标签激活/悬停/被拖时隐藏，始终占布局不回流；
    // 高度小于行高，上下留空隙不连通。
    auto tabDivider = [&](bool visible) {
        return huxerui::View{
            huxerui::Column{}.With(huxerui::Frame{.width = 1.0F, .height = 12.0F},
                                   huxerui::Background(theme.colors.outline),
                                   huxerui::Opacity(visible ? 1.0F : 0.0F))};
    };

    const TopTabId activeTab = activeTopTab.Get();

    // 主页标签 leading：home.svg 是固定 fill 的矢量资源，须用 Image::Tint 着色
    // （同下方齿轮/设置标签图标），否则深/浅模式下都是写死的黑色；
    // Fit(Contain)+Align 居中，避免画布大于 Frame 时从边缘锚定。
    // 未激活时用次级文字色，与文字标签的激活/未激活配色规则一致。
    const bool homeActive = activeTab == TopTabId{};
    const auto iconLeading = [&theme](const huxerui::ImageResource& resource, bool isActive) {
        return huxerui::View{huxerui::Image(resource)
                                 .Fit(huxerui::ImageFit::Contain)
                                 .Align(huxerui::HorizontalAlignment::Center,
                                        huxerui::VerticalAlignment::Center)
                                 .Tint(isActive ? theme.colors.on_surface
                                                : theme.colors.on_surface_variant)
                                 .With(huxerui::Frame{.width = 16.0F, .height = 16.0F})};
    };

    // 标签条内项目标签的渲染顺序 = tabs 顺序（打开顺序）；g_requests 只用于
    // 取名，已不存在的项目 id 跳过（与原逻辑一致）。
    std::vector<std::pair<std::int64_t, std::string>> visibleTabs;
    for (const std::int64_t id : tabs.Get()) {
        for (const db::Project& p : g_requests.allProjects()) {
            if (p.id == id) {
                visibleTabs.emplace_back(id, p.name);
                break;
            }
        }
    }

    // 条内标签描述（项目 + [设置单例]）；主页标签在 ScrollView 之外单独构建。
    struct Entry {
        TopTabId tab;
        std::int64_t key;  // 条内显示 key（TopTabDisplayKey）
        std::string name;  // 空 = 图标 only（本条内不会出现）
        huxerui::View leading;
        bool active;
        bool draggable;
    };
    std::vector<Entry> entries;
    for (const auto& [id, name] : visibleTabs) {
        const TopTabId tab{TopTabKind::Project, id};
        entries.push_back(Entry{tab, id, name, huxerui::View{huxerui::Row{}},
                                activeTab == tab, true});
    }
    // 设置单例标签：打开时渲染在项目标签条末尾（追加在所有项目标签之后，见函数
    // 头部布局决定）。外观与项目标签一致：齿轮图标 leading（Image::Tint 着色同
    // 主页图标）+ 文字「设置」+ 悬停显隐 ✕；可关闭；不持久化、不参与拖拽。
    const TopTabId settingsTab{TopTabKind::GlobalSettings, 0};
    if (settingsOpen.Get()) {
        entries.push_back(Entry{settingsTab, kSettingsTabDisplayKey, "设置",
                                iconLeading(app::images::gear, activeTab == settingsTab),
                                activeTab == settingsTab, false});
    }

    std::vector<huxerui::View> chips;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const Entry& entry = entries[i];
        // 拖拽几何（index/count）只对项目标签有意义：取项目标签在条内的下标/总数；
        // 主页/设置标签传 0/1（不挂 DragSource，不会被使用）。
        const bool isProject = entry.tab.kind == TopTabKind::Project;
        const std::size_t dragIndex = i;
        const std::size_t dragCount = visibleTabs.size();
        chips.push_back(
            TopTab(entry.tab, entry.name, entry.leading, entry.active, entry.draggable,
                   actions, tabs, dragId, dragDx, dragOrig, isProject ? dragIndex : 0,
                   isProject ? dragCount : 1, tabStride, slideCell, slideTick, hoveredTab)
                .Key(entry.key));
        // 标签间分隔竖线（最后一个不加）：相邻标签激活/悬停/被拖时隐藏，
        // 始终占布局（Opacity 显隐，不回流）。
        if (i + 1 < entries.size()) {
            const Entry& next = entries[i + 1];
            const bool sepVisible = !entry.active && !next.active &&
                                    hoveredTab.Get() != entry.key &&
                                    hoveredTab.Get() != next.key &&
                                    dragId.Get() != entry.key && dragId.Get() != next.key;
            chips.push_back(tabDivider(sepVisible));
        }
    }

    // 拖拽覆盖层：被拖标签的视觉克隆（纯展示，无 handler），X = 拖拽起点
    // 槽位 + 钳制后的累计位移，Y 恒 0。仅项目标签可拖（dragId 只含项目 id），
    // 覆盖层 ✕ 保持纯展示 Text（业务特例，与本体 AppIconButton 不同）。
    huxerui::View overlayTab = huxerui::Row{};
    if (dragId.Get() != 0 && !visibleTabs.empty()) {
        std::string dragName;
        for (const auto& [id, name] : visibleTabs) {
            if (id == dragId.Get()) dragName = name;
        }
        const bool dragActive =
            activeTab == TopTabId{TopTabKind::Project, dragId.Get()};
        const huxerui::Color overlayFill =
            dragActive ? theme.colors.surface_container_highest
                       : theme.colors.surface_container;
        const huxerui::Color overlayForeground =
            dragActive ? theme.colors.on_surface : theme.colors.on_surface_variant;
        const auto overlayFont =
            huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::SemiBold);
        // ✕ 与本体一致用常规字重（名称保持 SemiBold）。
        const auto overlayCloseFont = huxerui::Font::System(font_size::kChip);
        overlayTab =
            huxerui::Row {
                huxerui::Text(dragName, huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = overlayFont,
                                              .foreground = overlayForeground})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                          huxerui::Frame{.max_width = kProjectTabWidth - 32.0F},
                          huxerui::ClipChildren()),
                // 与本体一致：✕ 顶到右缘。
                huxerui::Spacer{},
                huxerui::Text("✕", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = overlayCloseFont,
                                              .foreground = overlayForeground})
                    .With(huxerui::Padding(4.0F)),
            }
                .With(huxerui::Spacing(0.0F), huxerui::Background(overlayFill),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                      huxerui::Frame{.width = kProjectTabWidth,
                                     .height = kTitleBarContentHeight},
                      huxerui::ClipChildren(),
                      huxerui::Offset(huxerui::Point{
                          static_cast<float>(dragOrig.Get()) * tabStride + dragDx.Get(),
                          0.0F}));
    }

    // 主页标签：图标 only，无文字，不可关闭，固定最左（在 ScrollView 之外）。
    huxerui::View homeTab =
        TopTab(TopTabId{}, "", iconLeading(app::images::home, homeActive), homeActive,
               /*draggable=*/false, actions, tabs, dragId, dragDx, dragOrig, 0, 1, tabStride,
               slideCell, slideTick, hoveredTab)
            .Key(std::int64_t{0});

    // 主页与条内首个标签之间的分隔竖线：主页或右邻激活、悬停、被拖时隐藏
    //（Opacity 显隐，始终占布局不回流）。
    bool homeDividerVisible = false;
    if (!entries.empty()) {
        const Entry& first = entries.front();
        homeDividerVisible = !homeActive && !first.active && hoveredTab.Get() != 0 &&
                             hoveredTab.Get() != first.key && dragId.Get() != first.key;
    }

    // 标签条按真实内容取自然上限，窄窗时仍接受父约束并由 ScrollView 横向滚动。
    // 不能让内部 ScrollView 的 Grow 把整个标题栏剩余宽度都占满，否则视觉上的
    // 大块空白仍会是 ScrollView/Client 命中，右侧也没有真正的拖动区。
    // 32 = 主页；其余按固定标签宽 + 分隔/间距的保守步进计算。
    const float naturalStripWidth =
        32.0F + 2.0F * theme.spacing.small + 1.0F +
        static_cast<float>(entries.size()) * tabStride;

    return huxerui::Row {
        std::move(homeTab),
        tabDivider(homeDividerVisible),
        // Stack 包裹：拖动时覆盖层克隆叠在标签行之上（绘制最上层），随
        // ScrollView 一起滚动（Offset 只平移绘制，布局原点仍在内容坐标系）。
        huxerui::ScrollView(huxerui::Stack {
                                huxerui::Row(std::move(chips))
                                    .With(huxerui::Spacing(theme.spacing.small),
                                          huxerui::CrossAlign(
                                              huxerui::CrossAxisAlignment::Center)),
                                std::move(overlayTab),
                            })
            .ScrollAxis(huxerui::Axis::Horizontal)
            .With(huxerui::ScrollBar{}, huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Frame{.max_width = naturalStripWidth});
}


// 左列：图标侧边栏（选中态用实心图标变体，悬停显示文字提示）。
// 去岛屿包裹：图标按钮直接落在窗口背景上；WebSocket/TCP 已并入请求页标签，不再单列。
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
        const huxerui::ImageResource& icon = navPage.Get() == page ? item.icon_selected : item.icon;
        buttons.push_back(
            huxerui::IconButton(icon, item.tooltip)
                .OnClick([tasks, navPage, page] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = page;
                    });
                })
                .With(huxerui::Tooltip(item.tooltip)));
    }
    return huxerui::Column(std::move(buttons))
        .With(huxerui::Padding(compact ? theme.spacing.small : theme.spacing.medium),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = compact ? 44.0F : 56.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// ---- 底部全局状态条与右缘弹窗 ----

// 状态栏摘要按码点截断（代理地址可能含非 ASCII）：数 UTF-8 首字节（非 10xxxxxx
// 续字节），超限截到上一个完整码点并补省略号。
std::string TruncateSummary(const std::string& text, std::size_t maxChars) {
    std::size_t chars = 0;
    std::size_t bytes = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) {
            if (chars >= maxChars) return text.substr(0, bytes) + "…";
            ++chars;
        }
        ++bytes;
    }
    return text; // 未超限：原样返回。
}

// 项目 Cookie 的行编辑缓冲元素：store 行（id/enabled）+ 完整 TextEditingValue
// （保留光标与选区，逐键回写不重置插入点）。
struct CookieRow {
    std::int64_t id = 0;
    huxerui::TextEditingValue name;
    huxerui::TextEditingValue value;
    bool enabled = true;

    // State<vector<CookieRow>> 写入去重需要值比较（TextEditingValue 自带 ==，
    // 同 draft.h KvRow）。
    bool operator==(const CookieRow&) const = default;
};

// 从 store 读当前项目 Cookie 列表 → 编辑缓冲（弹窗打开时取初值）。
std::vector<CookieRow> CookieRowsFromStore() {
    std::vector<CookieRow> rows;
    for (const db::GlobalCookie& cookie : g_requests.globalCookies()) {
        rows.push_back(CookieRow{cookie.id, huxerui::TextEditingValue{cookie.name},
                                 huxerui::TextEditingValue{cookie.value}, cookie.enabled});
    }
    return rows;
}

// 状态条右缘文字热区：kCaption 小字 + Padding 热区 + Tooltip + 点击开弹窗。
huxerui::View StatusActionText(std::string label, std::string tooltip,
                               const huxerui::ThemeSpec& theme,
                               std::function<void()> onClick) {
    return huxerui::Text(std::move(label), huxerui::TextRole::Label)
        .Style(huxerui::TextStyle{.font = huxerui::Font::System(font_size::kCaption),
                                  .foreground = theme.colors.on_surface_variant})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(theme.spacing.small, 2.0F)),
              huxerui::Tooltip(std::move(tooltip)))
        .OnClick(std::move(onClick));
}

// 请求代理弹窗：单行 Outlined TextField（初值 = 当前 request_proxy）+ 说明 +
// 取消/保存。保存 = saveSessionPreference("request_proxy")（存储键与 store 契约
// 一致：finalizeSpec/globalProxy() 读该键并自行 trim）+ toast + 关弹窗；
// bump version 让状态条摘要文本随重组刷新。写 KV/State 均不卸载子树，同步安全
// （约定 6 只约束会导致点击节点卸载的写）。
[[huxerui::composable]] huxerui::View RequestProxyDialogContent(
    huxerui::DialogContext ctx, huxerui::State<int> version) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    (void)version.Get(); // 订阅：弹窗存续期内外部 bump 时重组（text 缓冲不被初值重置）。
    std::string initial = trim(sessionPreference("request_proxy"));
    auto text = huxerui::UseState(huxerui::TextEditingValue{std::move(initial)});
    return DialogCard(huxerui::Column {
        huxerui::Text("请求代理", huxerui::TextRole::Title),
        huxerui::TextField(text)
            .Label("代理地址")
            .Placeholder("http://host:port 或 socks5://host:port")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([text](const huxerui::TextEditingValue& value) { text = value; }),
        huxerui::Text(
            "支持 http://host:port、socks5://host:port；作用于所有单次 HTTP 请求（curl "
            "引擎），k6 压测不受影响；留空 = 直连。",
            huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("保存").OnClick([ctx, text, version, toast] {
                const std::string proxy = trim(text.Get().text);
                saveSessionPreference("request_proxy", proxy);
                toast.Show(proxy.empty() ? "已清除请求代理（直连）" : "请求代理已保存");
                ctx.Dismiss();
                version = version.Get() + 1;
            }),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 380.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 项目 Cookie 管理弹窗（当前项目）。
// 编辑模型（选型说明）：行编辑态存 dialog 层 State<std::vector<CookieRow>> 缓冲，
// 行内保留完整 TextEditingValue——若逐键直通写库（OnChanged 即 saveGlobalCookie），
// 每字符一次 SQLite upsert、失败时 toast 刷屏，且重组回读 store 只剩纯文本会丢
// 光标。改为：文本改动只进缓冲，「保存」按钮统一落库（小表整表 upsert，不 diff；
// 名称非空才物化，空名行丢弃）；Checkbox 启用/禁用与 ✕ 删除是离散操作即点即落库
// （✕ 所在行会卸载，经 tasks.Launch + Delay(0) 推迟出指针事件路径，约定 6）。
// 先例：project_settings_page.cpp ProjectHeaderTable（同为缓冲 + 统一保存 + 虚拟末行）。
[[huxerui::composable]] huxerui::View GlobalCookieDialogContent(
    huxerui::DialogContext ctx, huxerui::State<int> version) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    (void)version.Get(); // 订阅：写库/删除 bump 后重组本弹窗（rows 缓冲保留，光标不重置）。
    std::vector<CookieRow> initial = CookieRowsFromStore(); // 初值在 UseState 之前算好（store 权威）。
    auto rows = huxerui::UseState(std::move(initial));

    const std::vector<CookieRow> data = rows.Get();
    std::vector<huxerui::View> children{
        huxerui::Row {
            huxerui::Text("", huxerui::TextRole::Label).With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text("名称", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
            huxerui::Text("值", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Foreground(theme.colors.on_surface_variant)),
    };
    for (std::size_t i = 0; i <= data.size(); ++i) {
        const bool phantom = i == data.size();
        const CookieRow row = phantom ? CookieRow{} : data[i];
        // 行写入缓冲：越界（虚拟行）时仅名称非空才物化追加。
        auto applyRow = [rows](std::size_t i, CookieRow updated) {
            std::vector<CookieRow> copy = rows.Get();
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.name.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            rows = copy;
        };
        // 离散操作即落库：先写缓冲再 upsert；新物化行回填 id。
        auto saveRow = [rows, toast, applyRow](std::size_t i, const CookieRow& updated) {
            applyRow(i, updated);
            if (updated.name.text.empty()) return;
            db::GlobalCookie cookie{updated.id, 0, updated.name.text, updated.value.text,
                                    updated.enabled};
            if (const std::string err = g_requests.saveGlobalCookie(cookie); !err.empty()) {
                toast.Show("保存 Cookie 失败: " + err);
                return;
            }
            if (cookie.id != updated.id) {
                std::vector<CookieRow> copy = rows.Get();
                if (i < copy.size()) {
                    copy[i].id = cookie.id;
                    rows = copy;
                }
            }
        };
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(row.enabled).OnChanged([row, i, saveRow](bool checked) {
                    CookieRow updated = row;
                    updated.enabled = checked;
                    saveRow(i, updated);
                }),
                huxerui::TextField(row.name)
                    .Label("名称")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        CookieRow updated = row;
                        updated.name = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(row.value)
                    .Label("值")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        CookieRow updated = row;
                        updated.value = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                phantom
                    ? huxerui::View{huxerui::Row{}.With(
                          huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                    : AppIconButton("✕", "删除 Cookie",
                          [tasks, rows, toast, version, row, i] {
                              // 删除会卸载 ✕ 所在行：推迟出指针事件路径（约定 6）；
                              // 落库删除同样在推迟任务里做，失败则保留缓冲行。
                              tasks.Launch([=]() -> huxerui::Task<void> {
                                  co_await huxerui::Delay(std::chrono::duration<double>{0});
                                  if (row.id != 0) {
                                      if (const std::string err =
                                              g_requests.deleteGlobalCookie(row.id);
                                          !err.empty()) {
                                          toast.Show("删除 Cookie 失败: " + err);
                                          co_return;
                                      }
                                  }
                                  std::vector<CookieRow> copy = rows.Get();
                                  if (i < copy.size()) {
                                      copy.erase(copy.begin() + static_cast<long>(i));
                                  }
                                  rows = copy;
                                  version = version.Get() + 1;
                              });
                          }, AppIconButtonShape::Bare),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }

    return DialogCard(huxerui::Column {
        huxerui::Text("项目 Cookie", huxerui::TextRole::Title),
        huxerui::Text("启用的项目 Cookie 在每次发送时并入请求；项目级静态值，不参与 "
                      "{{var}} 环境变量替换。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::ScrollView{huxerui::Column(std::move(children))
                                .With(huxerui::Spacing(theme.spacing.small),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar{}, huxerui::Frame{.max_height = 300.0F}),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("保存").OnClick([ctx, rows, toast, version] {
                // 缓冲统一落库：空名行不物化直接丢；有行失败则保持弹窗打开，
                // 成功行回填 id（不重复写）。
                std::vector<CookieRow> kept;
                bool failed = false;
                for (CookieRow r : rows.Get()) {
                    if (r.name.text.empty()) continue;
                    db::GlobalCookie cookie{r.id, 0, r.name.text, r.value.text, r.enabled};
                    if (const std::string err = g_requests.saveGlobalCookie(cookie);
                        !err.empty()) {
                        toast.Show("保存 Cookie 失败: " + err);
                        failed = true;
                        kept.push_back(std::move(r));
                        continue;
                    }
                    r.id = cookie.id;
                    kept.push_back(std::move(r));
                }
                version = version.Get() + 1;
                if (failed) {
                    rows = std::move(kept);
                } else {
                    ctx.Dismiss();
                }
            }),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 520.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 底部全局状态条（所有页面共享）：与侧边栏一样无岛屿包裹——无背景、无顶部分隔线
// （分隔线已删：根底色与上方岛屿已有层次，线条多余），一行小字直接落在窗口背景上，
// 右缘两个文字热区（请求代理 / 全局 Cookie）点击开弹窗。
// 独立成 provider 之下的 composable：弹窗层捕获调用处环境（CLAUDE.md），AppRoot
// 自身在 MinimalThemed provider 之上、UseTheme/UseDialog 只能拿默认浅色——热区
// 弹窗必须从这里 Show 才带正确主题（同 CloseGuard 的根因与做法）。
[[huxerui::composable]] huxerui::View GlobalStatusBar(float statusTopPad) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    // 重组触发器：代理/Cookie 弹窗落库后 bump，本作用域重读 sessionPreference，
    // 右缘代理摘要随之下一次组合刷新。
    auto version = huxerui::UseState(0);
    (void)version.Get();

    // 数据：以领域 store 为权威读取（组合期快照）。刷新时机：切页/切项目外壳重组、
    // 本组件随重组刷新；store 内部变化（环境切换、k6 探测结果翻转）不触发外壳重组，
    // 显示值待下一次自然重组更新（值低频变化，不为此新造订阅机制）。
    std::string statusProject = "未打开项目";
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == g_requests.currentProjectId()) {
            statusProject = p.name;
            break;
        }
    }
    std::string statusEnv = "无环境";
    if (const db::Environment* env = g_requests.findEnvironment(g_requests.currentEnvId())) {
        statusEnv = env->name;
    }
    // curl 引擎恒就绪（store 构造即持有）；k6 按引擎探测结果显示。
    const std::string statusK6 = g_loadtest.available() ? "k6: 就绪" : "k6: 未找到";
    const std::string proxy = trim(sessionPreference("request_proxy"));
    const std::string proxyLabel =
        "应用代理: " + (proxy.empty() ? std::string("无") : TruncateSummary(proxy, 24));
    const auto statusText = [&theme](std::string text) {
        return huxerui::Text(std::move(text), huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{
                .font = huxerui::Font::System(font_size::kCaption),
                .foreground = theme.colors.on_surface_variant});
    };

    return huxerui::Row {
        statusText(statusProject),
        statusText(statusEnv),
        statusText("HTTP: curl"),
        statusText(statusK6),
        // 右缘占位：空 Row + Grow(1.0F) 吃掉剩余宽度，把两个热区推到行尾。
        // 约定 8：零尺寸占位不用 Spacer（自带 Grow(1) 的隐式语义留给真弹性项）。
        huxerui::Row{}.With(huxerui::Grow(1.0F)),
        StatusActionText(proxyLabel, "设置应用级代理（curl 引擎，k6 压测不受影响）",
                         theme,
                         [dialog, version] {
                             // 开弹窗是层操作、不卸载按钮子树，指针事件路径上同步 Show
                             // 安全（先例：树行右键直接 ShowPopupMenuAt）。
                             dialog.Show(
                                 [version](huxerui::DialogContext ctx) mutable -> huxerui::View {
                                     return RequestProxyDialogContent(ctx, version);
                                 },
                                 huxerui::DialogOptions{});
                         }),
        StatusActionText("项目 Cookie", "管理当前项目的项目 Cookie", theme,
                         [dialog, version] {
                             dialog.Show(
                                 [version](huxerui::DialogContext ctx) mutable -> huxerui::View {
                                     return GlobalCookieDialogContent(ctx, version);
                                 },
                                 huxerui::DialogOptions{});
                         }),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              // 左右留白与侧栏/页面边距对齐（spacing.medium）；顶部补偿 padding 补回
              // 根 Column 收窄的间隙：主行↔状态条保持 gap 不变。
              huxerui::Padding(huxerui::EdgeInsets{.top = statusTopPad,
                                                   .right = theme.spacing.medium,
                                                   .bottom = 0.0F,
                                                   .left = theme.spacing.medium}),
              huxerui::Frame{.height = 22.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace

// 关闭询问弹窗宿主：必须在 MinimalThemed provider 之下组合——AppRoot 自身在
// provider 之上，层内容捕获调用处环境，在 AppRoot 里 dialog.Show 的弹窗
// UseTheme() 只能拿到默认浅色 spec（弹窗不应用主题的根因）。关闭拦截
// （OnCloseRequest）与询问弹窗都挂在这里；content 原样返回，仅附加行为。
[[huxerui::composable]] huxerui::View CloseGuard(huxerui::State<int> closeBehavior,
                                                 huxerui::State<bool> closeDialogOpen,
                                                 huxerui::View content) {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    auto dialog = huxerui::UseDialog();
    auto tasks = huxerui::UseTaskScope();

    // 关闭拦截：按配置直接关闭/进托盘；未配置时第一次询问并把选择写入配置。
    // 托盘可用性在关闭时动态查询（不要用组合期快照，避免错过宿主晚就绪）。
    // Hide 一律经 tasks.Launch + Delay(0) 推迟到事件派发之后：在关闭请求回调里
    // 同步隐藏最后一个可见窗口，会让 SDK 运行时在回调栈上就地进行窗口拆毁/
    // 退出路径（回调返回后栈上的 WindowService 已被回收）——用户实测直接崩溃。
    // 先同步 return true 拦下关闭，再在事件循环外 Hide，行为不变、栈安全。
    auto hideToTray = [tasks, window] {
        tasks.Launch([window]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            window.Hide();
        });
    };
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (!tray.IsAvailable()) return false; // 无托盘：交给系统直接关闭
            if (closeBehavior.Get() == 1) return false;
            if (closeBehavior.Get() == 2) {
                hideToTray();
                return true;
            }
            if (closeDialogOpen.Get()) return true;
            closeDialogOpen = true;
            // 自定义内容弹窗（DialogCard 包底板）：内置两按钮弹窗没有取消入口。
            // 三个按钮：直接关闭 / 最小化到托盘 / 取消；按钮点击在弹层指针事件
            // 路径上，Quit/Hide 等全局副作用推迟到事件派发之后（CLAUDE.md 约定 6）。
            dialog.Show(
                [=](huxerui::DialogContext ctx) mutable -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("关闭 apitab？", huxerui::TextRole::Title),
                        huxerui::Text("直接退出应用，还是最小化到系统托盘继续运行？",
                                      huxerui::TextRole::Body),
                        huxerui::Row {
                            huxerui::Button("直接关闭")
                                .OnClick([=]() mutable {
                                    closeDialogOpen = false;
                                    closeBehavior = 1;
                                    saveSessionPreference("close_behavior", "1");
                                    ctx.Dismiss();
                                    tasks.Launch([application]() -> huxerui::Task<void> {
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        application.Quit();
                                    });
                                }),
                            huxerui::Button("最小化到托盘")
                                .OnClick([=]() mutable {
                                    closeDialogOpen = false;
                                    closeBehavior = 2;
                                    saveSessionPreference("close_behavior", "2");
                                    ctx.Dismiss();
                                    hideToTray();
                                }),
                            huxerui::Button("取消").OnClick([=]() mutable {
                                closeDialogOpen = false;
                                ctx.Dismiss();
                            }),
                        }
                            // 两端对齐：与其他弹窗一致（取消类在右、动作在左）。
                            .With(huxerui::Spacing(8.0F),
                                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                                          .With(huxerui::Spacing(12.0F),
                                                huxerui::Frame{.width = 360.0F},
                                                huxerui::CrossAlign(
                                                    huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{
                    .dismiss_on_outside_press = false,
                    .dismiss_on_cancel = false,
                });
            return true;
        },
        0);

    return content;
}

// 应用根：状态全部在这里（官方 README 形态：根标注 composable）。
[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    // IsAvailable() 内部会观察托盘可用性 State：DBus 托盘宿主就绪较晚时，
    // 这里在组合期订阅，可用性翻转后本作用域重组、托盘随后注册。
    const bool trayAvailable = tray.IsAvailable();
    auto tasks = huxerui::UseTaskScope();

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色，未保存偏好时默认深色（极简黑白黑底）。
    int initialThemeMode = 1;
    if (sessionPreference("theme_mode") == "0") initialThemeMode = 0;
    if (sessionPreference("theme_mode") == "2") initialThemeMode = 2;
    // 关闭行为：0=每次询问 1=直接关闭 2=最小化到托盘
    int initialCloseBehavior = 0;
    if (sessionPreference("close_behavior") == "1") initialCloseBehavior = 1;
    if (sessionPreference("close_behavior") == "2") initialCloseBehavior = 2;

    // ---- 顶级标签状态（island-structure-theme.md §13.1/§13.2，P1-B0.1）----
    // navPage：项目工作区内部页（kRequest/kLoad/kHistory/kProjectSettings + 遗留
    //   kHttpTest），不再包含 kHome/kAppSettings 等顶级目的地。
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
        // 遗留 HttpTest（navPage=kHttpTest，标题栏闪电入口）不是项目工作区页面：
        // 任何顶级激活都退出它，避免「顶级标签已切换仍停在 HttpTest」的双状态残留。
        if (navPage.Get() == pages::kHttpTest) navPage = pages::kRequest;
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
        if (!(before.active == after.active) && navPage.Get() == pages::kHttpTest) {
            // 同 activateTopTabNow：活动顶级标签变化后退出遗留 HttpTest 全宽页
            //（它不属于任何顶级标签）；关非活动标签不动当前页面。
            navPage = pages::kRequest;
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
    const huxerui::ThemeSpec rootSpec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    // 响应式：Compact(<600) 收窄间距，Medium/Expanded 保持现状。
    // 根 Column 子项间隙统一 extra_small(4pt)：标题栏↔主行贴紧一些；
    // 主行↔状态条由状态条顶部补偿 padding 补回 gap，维持原间距不变。
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
    // → 侧栏 + PageFor（项目工作区内部页，navPage 表达）。遗留 HttpTest
    //（navPage=kHttpTest，闪电入口）优先显示且全宽——它不表达顶级标签（§13.1：
    // P1-B1 项 4 再决定其顶级身份），任何顶级激活都会重置 navPage（见
    // activateTopTabNow）。防御：active 项目 id 不在 tabs（open_projects）时回落
    // 主页——该路径不应发生（activeTopTab 只经 ActivateProject/CloseTopTab 变更，
    // 不变量 1 保证 active 项目在 open_projects 中）。
    const TopTabId activeTab = activeTopTab.Get();
    const bool legacyHttpTest = navPage.Get() == pages::kHttpTest;
    bool showSideShell = false;
    huxerui::View page;
    if (legacyHttpTest) {
        page = huxerui::View{HttpTestPage()}.Key(std::string{"httptest"}).With(huxerui::Grow(1.0F));
    } else {
        // P1-B0.5 状态保活：顶级标签内容用 IndexedPages 保持所有页面挂载（设置↔项目切换不卸载，草稿保活）
        std::vector<huxerui::View> indexed;
        indexed.reserve(1 + tabs.Get().size() + (settingsOpen.Get() ? 1 : 0));
        indexed.push_back(HomePage(onOpenProject, activeProject).Key(std::int64_t{0}).With(huxerui::Grow(1.0F)));
        std::unordered_map<std::int64_t, std::size_t> projIdx;
        for (std::int64_t id : tabs.Get()) {
            const std::size_t idx = indexed.size();
            projIdx[id] = idx;
            indexed.push_back(pages::PageFor(navPage.Get(), activeProject).Key(id).With(huxerui::Grow(1.0F)));
        }
        if (settingsOpen.Get()) {
            indexed.push_back(GlobalSettingsPage(themeMode, closeBehavior, settingsCategory)
                                   .Key(kSettingsTabDisplayKey)
                                   .With(huxerui::Grow(1.0F)));
        }
        std::size_t selected = 0;
        if (activeTab.kind == TopTabKind::Home) {
            selected = 0;
        } else if (activeTab.kind == TopTabKind::Project) {
            const auto it = projIdx.find(activeTab.project_id);
            if (it != projIdx.end()) {
                selected = it->second;
                showSideShell = true;
            } else {
                selected = 0;
            }
        } else if (activeTab.kind == TopTabKind::GlobalSettings) {
            selected = indexed.size() > 0 ? indexed.size() - 1 : 0;
        }
        if (selected >= indexed.size()) selected = 0;
        page = huxerui::IndexedPages(std::move(indexed), selected);
    }
    // IndexedPages keep-alive（P1-B0.5）：切主题只是根重组、IndexedPages 保持所有页面挂载
    // （设置↔项目切换不卸载，草稿与设置分类保活）；切项目/切内部页仅切换 selected 索引。

    huxerui::View content = huxerui::Column {
        // 自定义标题栏岛：Logo + 顶级标签条 + 闪电/齿轮（框架在其右侧渲染窗口按钮）。
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
            // 闪电：框架 HTTP 协程压测实验页（遗留顶级入口，保持现状：写
            // navPage=kHttpTest 全宽显示，不表达顶级标签）。§13.1 要求记录后续
            // 选择：要么升级为同类单例顶级标签，要么改为 Dialog/工具窗口
            //（P1-B1 项 4 评估）；禁止继续新增把它当项目页面 navPage 的入口。
            // 与齿轮同款裸 Image 热区（不写背景、24pt 等高垂直居中、Tint 跟随
            // 深浅色）。
            huxerui::Row {
                huxerui::Image(app::images::bolt)
                    .Fit(huxerui::ImageFit::Contain)
                    .Align(huxerui::HorizontalAlignment::Center,
                           huxerui::VerticalAlignment::Center)
                    .Tint(rootSpec.colors.on_surface)
                    .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                      huxerui::Frame{.height = kTitleBarContentHeight},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Tooltip("HTTP 协程压测"),
                      // 键盘/语义（P1-B0.4）：裸 Image 热区默认不可聚焦，补
                      // Focusable + Button 语义后键盘可激活（§13.6）。
                      huxerui::Focusable(true),
                      huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                         .label = "HTTP 协程压测"})
                .OnClick([tasks, navPage] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = pages::kHttpTest;
                    });
                }),
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
                      // 键盘/语义（P1-B0.4）：同闪电按钮——键盘可打开/激活设置
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
        }
            // 标签栏收窄 + 去背景：直接融入窗口底色，不再垫 surface_container_low。
            // 垂直零内边距：内容本身 24pt 高，与 title_bar_height 对齐，避免
            // 标题栏下缘与岛屿之间多出一条空隙。
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small, 0.0F)),
                  huxerui::Spacing(gap)),
        // 主行：侧栏（无岛屿包裹）+ 内容区；Grow 吃满标题栏之外的剩余高度。
        // 内容区不再套外壳岛：区域划分由各页面自己的一级岛屿承担，避免双层嵌套。
        // 仅 Project(id) 顶级标签显示侧栏；主页/设置/遗留 HttpTest 整宽覆盖。
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
        // 底部全局状态条（所有页面共享）：独立 composable（结构与主题宿主说明见
        // GlobalStatusBar）——无顶部分隔线，一行小字 + 右缘两个文字热区
        // （应用代理 / 项目 Cookie）。
        GlobalStatusBar(statusTopPad),
    }
                               .With(huxerui::Spacing(rootSpec.spacing.extra_small),
                                     // 窗口整体背景：主题背景色刷满根节点，
                                     // 否则深色模式下岛间缝隙透出窗口默认白底。
                                     huxerui::Background(rootSpec.colors.background),
                                     // 交叉轴必须 Stretch：否则主行（侧栏+页面区）按内容
                                     // 收缩成内容宽，页面里所有 Grow 失去上界、岛屿无法
                                     // 占满逻辑区块（首页整体漂移/右对齐的根因）。
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 主题边界走 MinimalThemed：自定义 spec + 组件 typed style 覆盖（8px 按钮圆角）。
    // 关闭询问弹窗宿主挂在 provider 之下（AppRoot 自身读不到主题，CloseGuard 能）。
    return MinimalThemed(dark, CloseGuard(closeBehavior, closeDialogOpen, std::move(content)));

}

} // namespace apitab::ui
