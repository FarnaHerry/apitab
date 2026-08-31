// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘）：
//   标题栏岛：Logo(AT) + 顶级项目标签条（主页钉在最左，项目标签横向滚动）+ 齿轮
//     (全局设置) + 框架窗口按钮；收窄为 32px 高、去背景直接融入窗口底色，
//     主题为极简 AI 黑白风（MinimalDark/MinimalLightThemeSpec，对齐 tinynext 配色）。
//   下方：左侧图标侧边栏（无岛屿包裹，直接落在窗口背景上）｜内容区（页面自己的
//   一级岛屿划分区域，外壳不再套岛）。根节点刷整窗底色（rootSpec.colors.background——
//   AppRoot 在主题 provider 之上，UseTheme 只能拿到默认浅色 spec，须按 dark 自选）。
//   响应式：UseViewportClass() Compact 时收窄侧栏宽度与各处间距。
// 托盘：托盘图标/菜单（显示主窗口/退出）；关闭行为三选（每次询问/直接关闭/
//   最小化到托盘），未配置时第一次关闭弹窗询问并把选择写入配置。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

namespace apitab::ui {

namespace pages {

enum PageIndex : std::size_t {
    kHome = 0,
    kRequest = 1,
    kLoad = 2,
    kWebSocket = 3,
    kTcp = 4,
    kHistory = 5,
    kProjectSettings = 6,
    kAppSettings = 7,
    kHttpTest = 8, // 框架 HTTP 协程压测实验页（标题栏闪电图标进入）
};

[[huxerui::composable]] huxerui::View PageFor(std::size_t index,
                                              huxerui::State<std::size_t> navPage,
                                              huxerui::State<std::vector<std::int64_t>> tabs,
                                              huxerui::State<std::int64_t> activeProject,
                                              huxerui::State<int> themeMode,
                                              huxerui::State<int> closeBehavior) {
    switch (index) {
        case kHome:
            return HomePage(navPage, tabs, activeProject);
        case kRequest:
            // 主页已整宽覆盖侧栏：未打开项目时本页不可达，无需兜底。
            return RequestPage(activeProject);
        case kLoad:
            return LoadTestPage();
        // kWebSocket/kTcp 不再可达：已并入请求页内部标签（PageIndex 枚举值保留）。
        case kHistory:
            return HistoryPage();
        case kProjectSettings:
            return ProjectSettingsPage();
        case kHttpTest:
            return HttpTestPage();
        default:
            return GlobalSettingsPage(themeMode, closeBehavior);
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
// 固定宽标签。主页图标标签不在此列（32pt 见 ProjectTab）。
constexpr float kProjectTabWidth = 140.0F;

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
    huxerui::ThemeSpec spec; // 默认值即内置浅色方案
    spec.colors.primary = huxerui::Color::Rgb(0, 0, 0);            // 纯黑主色
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);   // 黑底上翻白
    spec.colors.secondary = huxerui::Color::Rgb(69, 69, 71);
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(229, 229, 232);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(31, 31, 31);
    spec.colors.background = huxerui::Color::Rgb(244, 244, 245);   // #F4F4F5 近白
    spec.colors.surface = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container_low = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container = huxerui::Color::Rgb(240, 240, 241);
    spec.colors.surface_container_high = huxerui::Color::Rgb(231, 231, 233);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(222, 222, 225);
    spec.colors.on_surface = huxerui::Color::Rgb(31, 31, 31);      // 0.12 近黑
    spec.colors.on_surface_variant = huxerui::Color::Rgb(115, 115, 120); // 0.45 灰
    spec.colors.outline = huxerui::Color::Rgb(201, 201, 204);
    spec.colors.inverse_surface = huxerui::Color::Rgb(31, 31, 31);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(255, 255, 255);
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

// 软件徽标：字母 AT 合成的圆角块。与标题栏所有控件统一 24pt 高（kTitleBarHeight）。
[[huxerui::composable]] huxerui::View LogoBadge() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text("AT", huxerui::TextRole::Title)
        .With(huxerui::Frame{.width = 32.0F, .height = kTitleBarContentHeight},
              huxerui::Background(theme.colors.primary),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::Foreground(theme.colors.on_primary),
              huxerui::Align{.horizontal = huxerui::HorizontalAlignment::Center,
                             .vertical = huxerui::VerticalAlignment::Center});
}

// 顶级标签拖拽载荷：按项目 id 定位源/目标标签（主页标签 id=0 不参与拖拽，
// 位置恒定最左）。
struct ProjectTabDragPayload {
    std::int64_t projectId = 0;
};

// 单个标签页：激活态 = 最高层级容器底 + 主文字色；未激活 = 略深容器底 + 次级文字色。
// 整块外层只负责激活与切换（点击卸载内容子树，推迟出指针事件路径，CLAUDE.md 约定 6）；
// 内层用两个兄弟节点分别承载“切换”与“关闭”，避免各自做一次整标签的背景重绘。
// 项目标签（id != 0）另挂拖拽换位；主页标签（id == 0）不挂，位置恒定最左。
// 拖拽的 strip 级状态（dragId/dragDx/dragOrig）与几何（index/count/stride）
// 由 ProjectTabStrip 传入：拖动时本标签变透明占位，视觉由条内覆盖层克隆接管。
[[huxerui::composable]] huxerui::View ProjectTab(
    huxerui::State<std::size_t> navPage, huxerui::State<std::vector<std::int64_t>> tabs,
    huxerui::State<std::int64_t> activeProject, std::int64_t id, huxerui::View leading,
    std::string name, huxerui::State<std::int64_t> dragId, huxerui::State<float> dragDx,
    huxerui::State<int> dragOrig, std::size_t index, std::size_t count, float stride,
    huxerui::State<std::shared_ptr<SlideCell>> slideCell,
    huxerui::State<std::uint64_t> slideTick,
    huxerui::State<std::int64_t> hoveredTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    const bool active = activeProject.Get() == id;
    // 悬停才显示 ✕ 与背景。官方 view 级 Hover 事件（containment 生命周期）：
    // 指针进入标签呈现边界 Enter、真正离开才 Leave，在子组件（切换区/✕）之间
    // 移动不触发 Leave，天然等价于原来三个 HoverTrack 共享 cell 的聚合语义。
    // 同步写 strip 级 hoveredTab（分隔竖线显隐要用；-1 = 无，0 是主页 id）。
    auto hovered = huxerui::UseState(false);
    // 拖动中（仅本标签被拖时）变透明占位：保留布局槽位与拖拽会话，视觉由
    // ProjectTabStrip 的覆盖层克隆接管——覆盖层无任何事件 handler，命中测试
    // 穿透到下方静止标签。
    const bool dragging = id != 0 && dragId.Get() == id;

    // 标题栏已无底色（融入窗口背景）；标签背景默认也不显示（透明）——
    // 激活 = surface_container_highest 提亮，悬停 = surface_container 浮起，
    // 常态下只靠标签间的竖线分隔。
    const huxerui::Color tabFill =
        active ? theme.colors.surface_container_highest
               : (hovered.Get() ? theme.colors.surface_container
                                : huxerui::Color::Transparent());
    const huxerui::Color tabForeground =
        active ? theme.colors.on_surface : theme.colors.on_surface_variant;

    auto activateProject = [tasks, activeProject, tabs, navPage](std::int64_t tabId) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            if (tabId != 0) {
                g_requests.selectProject(tabId);
                g_loadtest.setProject(tabId);
                saveSessionPreference("active_project", std::to_string(tabId));
                if (navPage.Get() == pages::kHome) navPage = pages::kRequest; // 进入工作区
            } else {
                navPage = pages::kHome; // 主页标签
            }
            activeProject = tabId;
        });
    };
    auto closeTab = [tasks, activeProject, tabs, navPage](std::int64_t tabId) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            std::vector<std::int64_t> rest = tabs.Get();
            std::erase(rest, tabId);
            tabs = rest;
            if (activeProject.Get() == tabId) {
                g_requests.selectProject(0);
                g_loadtest.setProject(0);
                saveSessionPreference("active_project", "0");
                activeProject = 0;
                navPage = pages::kHome;
            }
        });
    };

    const auto badgeFont =
        huxerui::Font::System(font_size::kChip).WithWeight(huxerui::FontWeight::SemiBold);
    // ✕ 用常规字重（与请求页 chip、行内操作符一致）；badgeFont 的 SemiBold
    // 只留给标签名称，粗体叉笔画糊成一团难看。
    const auto closeFont = huxerui::Font::System(font_size::kChip);
    // 主页标签只放图标：省略文字、收窄边距并固定窄宽，避免挤占项目标签空间；
    // 限宽与裁剪只压内部「切换区」（图标+文字），长项目名截断而行尾 ✕ 永远完整显示。
    // 所有标签统一 kTitleBarContentHeight 高；内外两层 Row 都交叉轴居中，
    // 图标/文字/✕ 不会在 24pt 条里各自顶格漂移。
    const bool iconOnly = name.empty();
    huxerui::View tab = huxerui::Row {
        // 切换区：点击 = 激活本标签。max_width 给行尾 ✕ 留出位置
        // （固定宽 140 内：切换区 ≤120 + ✕ ≈16 + 间隙）。
        huxerui::Row {std::move(leading),
                      iconOnly
                          ? huxerui::View{huxerui::Row{}}
                          : huxerui::View{huxerui::Text(name, huxerui::TextRole::Label)
                                              .Style(huxerui::TextStyle{
                                                  .font = badgeFont,
                                                  .foreground = tabForeground})}}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                  huxerui::Spacing(4.0F),
                  huxerui::Frame{.max_width = kProjectTabWidth - 20.0F},
                  huxerui::ClipChildren(),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  // 整标签的悬停/选中反馈由外层 tabFill 承担；显式空 Indication
                  // 压掉 OnClick 的默认高亮，避免名称区再叠一层。
                  huxerui::Indication{})
            .OnClick([activateProject, id] { activateProject(id); }),
        // 弹性占位把 ✕ 顶到固定宽标签的右缘（Spacer 自带 Grow(1)）。
        id != 0 ? huxerui::View{huxerui::Spacer{}} : huxerui::View{huxerui::Row{}},
        // 关闭区（仅项目标签）：常驻、透明占位，悬停（本标签任一部分）才显示；
        // Opacity 只改绘制不动结构，避免悬停重组换子节点类型引起抖动。
        // 透明时点击空转。
        id != 0
            ? huxerui::View{huxerui::Text("✕", huxerui::TextRole::Label)
                                .Style(huxerui::TextStyle{
                                    .font = closeFont,
                                    .foreground = tabForeground,
                                })
                                .With(huxerui::Padding(4.0F),
                                      huxerui::Opacity(hovered.Get() ? 1.0F : 0.0F))
                                .OnClick([closeTab, id, visible = hovered.Get()] {
                                    if (!visible) return; // 透明占位不响应点击
                                    closeTab(id);
                                })}
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
              huxerui::Offset(huxerui::Point{SlideOffsetOf(slideCell.Get(), id), 0.0F}))
        // 悬停（containment）：进入边界置位，真正离开才清除；Leave 仅当
        // hoveredTab 仍是本标签才清 -1，避免竞态清掉邻居刚写入的 Enter。
        .On<huxerui::ViewEvents::Hover>(
            [hovered, hoveredTab, id](const huxerui::HoverEvent& e) {
                if (e.type == huxerui::HoverEventType::Enter) {
                    hovered = true;
                    hoveredTab = id;
                } else if (e.type == huxerui::HoverEventType::Leave) {
                    hovered = false;
                    if (hoveredTab.Get() == id) hoveredTab = -1;
                }
            });

    // 项目标签拖拽换位（主页标签 id==0 不挂任何拖拽修饰符，位置恒定最左）。
    // 限水平轴（axis=Horizontal）：标签只在标签条行内移动，竖向拖出不进拖拽。
    // Chrome 式贴条滑动：无悬浮拖影；Changed 回写钳制后的 X 位移（范围 =
    // 本标签到条内容两端，拖到容器外贴边停住而不是被裁掉），驱动
    // ProjectTabStrip 的覆盖层克隆。重排后持久化 open_projects。
    // 注：设计上预留「竖向拖出 = 拆成独立窗口、可拖回」，但 SDK 0.1.0 是单窗口
    // 模型（UseWindow 只有主窗口命令，无新建窗口 API；拖放是单视图树内
    // in-process），待 SDK 支持多窗口后把这里改回二维拖拽并加拆出逻辑。
    // 完整方案与检索规则见 docs/plans/project-tab-tear-off-window.md。
    if (id != 0) {
        tab = std::move(tab)
                  .With(huxerui::DragSource(
                      ProjectTabDragPayload{id},
                      huxerui::DragGesture{.axis = huxerui::Axis::Horizontal}))
                  // 拖拽开始：记录被拖标签与起点槽位。
                  .On<huxerui::DragSourceEvents::Started>(
                      [dragId, dragOrig, id, index](const huxerui::DragEvent&) {
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
                      [dragId, dragDx, dragOrig, tabs, tasks, slideCell, slideTick, id, count,
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
                          const auto from =
                              static_cast<std::size_t>(std::distance(copy.begin(), fromIt));
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
    return tab;
}

// 顶级标签条：主页标签（房子图标，固定不可关）钉在最左不参与滚动；
// 其余每个打开的项目一个可关标签，放进横向 ScrollView，溢出时滚动而不是挤变形。
[[huxerui::composable]] huxerui::View ProjectTabStrip(
    huxerui::State<std::size_t> navPage, huxerui::State<std::vector<std::int64_t>> tabs,
    huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 拖拽中的 strip 级状态：被拖标签 id + 钳制后的累计 X 位移 + 起点槽位
    // （见 ProjectTab）。拖动时被拖标签本体透明占位，视觉由下方覆盖层克隆
    // 接管（Stack 最后声明 = 绘制最上层；克隆无事件 handler，命中测试穿透
    // 到下方静止标签）。
    auto dragId = huxerui::UseState<std::int64_t>(0);
    auto dragDx = huxerui::UseState(0.0F);
    // 拖动起点槽位（Started 时记录）：覆盖层 X = 起点槽位 + 累计位移，
    // 与实时换位后的数据下标解耦，视觉连续不跳变。
    auto dragOrig = huxerui::UseState(0);
    // 让位滑动（StartSlide/SlideCell 见 ui.h）：tick 仅作重组触发器。
    auto slideCell = huxerui::UseState(std::make_shared<SlideCell>());
    auto slideTick = huxerui::UseState<std::uint64_t>(0);
    (void)slideTick.Get(); // 订阅：tween 每步 bump 触发重组
    // 悬停标签 id（-1 = 无；主页 id=0 也会写入）：分隔竖线显隐用。
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

    // 主页标签：图标 only，无文字。home.svg 是固定 fill 的矢量资源，
    // 须用 Image::Tint 着色（同下方齿轮），否则深/浅模式下都是写死的黑色；
    // Fit(Contain)+Align 居中，避免画布大于 Frame 时从边缘锚定。
    // 未激活时用次级文字色，与文字标签的激活/未激活配色规则一致。
    const bool homeActive = activeProject.Get() == 0;
    huxerui::View homeTab =
        ProjectTab(navPage, tabs, activeProject, 0,
                   huxerui::View{huxerui::Image(app::images::home)
                                     .Fit(huxerui::ImageFit::Contain)
                                     .Align(huxerui::HorizontalAlignment::Center,
                                            huxerui::VerticalAlignment::Center)
                                     .Tint(homeActive ? theme.colors.on_surface
                                                      : theme.colors.on_surface_variant)
                                     .With(huxerui::Frame{.width = 16.0F, .height = 16.0F})},
                   "", dragId, dragDx, dragOrig, 0, 1, tabStride, slideCell, slideTick,
                   hoveredTab)
            .Key(std::int64_t{0});

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
    // 覆盖层克隆所需：被拖标签的名称 / 配色，在构建 chips 时一并记录
    // （位置用 dragOrig，不用实时下标——换位后下标变化但视觉要连续）。
    std::string dragName;
    bool dragActive = false;
    std::vector<huxerui::View> chips;
    for (std::size_t index = 0; index < visibleTabs.size(); ++index) {
        const auto& [id, name] = visibleTabs[index];
        if (dragId.Get() == id) {
            dragName = name;
            dragActive = activeProject.Get() == id;
        }
        chips.push_back(ProjectTab(navPage, tabs, activeProject, id,
                                   huxerui::View{huxerui::Row{}}, name, dragId, dragDx,
                                   dragOrig, index, visibleTabs.size(), tabStride, slideCell,
                                   slideTick, hoveredTab)
                            .Key(id));
        // 标签间分隔竖线（最后一个不加）：相邻标签激活/悬停/被拖时隐藏，
        // 始终占布局（Opacity 显隐，不回流）。
        if (index + 1 < visibleTabs.size()) {
            const std::int64_t nextId = visibleTabs[index + 1].first;
            const bool sepVisible = activeProject.Get() != id &&
                                    activeProject.Get() != nextId &&
                                    hoveredTab.Get() != id && hoveredTab.Get() != nextId &&
                                    dragId.Get() != id && dragId.Get() != nextId;
            chips.push_back(tabDivider(sepVisible));
        }
    }

    // 拖拽覆盖层：被拖标签的视觉克隆（纯展示，无 handler），X = 拖拽起点
    // 槽位 + 钳制后的累计位移，Y 恒 0。
    huxerui::View overlayTab = huxerui::Row{};
    if (dragId.Get() != 0 && !visibleTabs.empty()) {
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

    return huxerui::Row {
        std::move(homeTab),
        // 主页与第一个项目标签之间的分隔竖线：无项目标签 / 主页或右邻
        // 激活、悬停、被拖时隐藏（Opacity 显隐，始终占布局不回流）。
        tabDivider(!visibleTabs.empty() && activeProject.Get() != 0 &&
                   hoveredTab.Get() != 0 &&
                   activeProject.Get() != visibleTabs.front().first &&
                   hoveredTab.Get() != visibleTabs.front().first &&
                   dragId.Get() != visibleTabs.front().first),
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
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
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

    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    // 顶级标签：activeProject = 0 为主页标签；其余值为打开的项目 id。
    auto tabs = huxerui::UseState<std::vector<std::int64_t>>({});
    auto activeProject = huxerui::UseState<std::int64_t>(0);
    auto closeBehavior = huxerui::UseState<int>(std::move(initialCloseBehavior));
    auto closeDialogOpen = huxerui::UseState(false);

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

    // 底部全局状态条数据：以领域 store 为权威读取（组合期快照）。
    // 刷新时机：AppRoot 作用域已订阅 navPage/activeProject（下方主行与 Key 读取），
    // 切页/切项目时外壳重组、状态条随之刷新；store 内部变化（如请求页内切换
    // 环境、k6 探测结果翻转）不触发外壳重组，显示值待下一次自然重组时更新——
    // 参照 request_page 的 listVersion/envVersion 需页面级 State，此处不为
    // 状态条新造订阅机制（值低频变化，代价不值得）。
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
    const auto statusText = [&rootSpec](std::string text) {
        return huxerui::Text(std::move(text), huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{
                .font = huxerui::Font::System(font_size::kCaption),
                .foreground = rootSpec.colors.on_surface_variant});
    };

    huxerui::View content = huxerui::Column {
        // 自定义标题栏岛：Logo + 项目标签条 + 齿轮（框架在其右侧渲染窗口按钮）。
        // 全部内容统一 24pt 高（kTitleBarContentHeight = title_bar_height）；
        // WindowTitleBar 构造即带交叉轴居中，这里给中间标签条包装 Row 也补上
        // 居中，任何一侧偏高都不漂移。
        huxerui::WindowTitleBar {
            LogoBadge(),
            huxerui::Row {ProjectTabStrip(navPage, tabs, activeProject)}
                .With(huxerui::Grow(1.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
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
            // 闪电：框架 HTTP 协程压测实验页。与齿轮同款裸 Image 热区（不写
            // 背景、24pt 等高垂直居中、Tint 跟随深浅色）。
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
                      huxerui::Tooltip("HTTP 协程压测"))
                .OnClick([tasks, navPage] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = pages::kHttpTest;
                    });
                }),
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
                      huxerui::Tooltip("全局设置"))
                .OnClick([tasks, navPage] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = pages::kAppSettings;
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
        // 主页与全局设置（都与项目无关）内容整宽覆盖侧栏：未打开项目就点不到任何
        // 项目相关入口。占位必须用空 Row——Spacer 自带 Grow(1)，会分走一半宽度。
        huxerui::Row {
            navPage.Get() == pages::kHome || navPage.Get() == pages::kAppSettings ||
                    navPage.Get() == pages::kHttpTest
                ? huxerui::View{huxerui::Row{}}
                : SideShell(navPage),
            pages::PageFor(navPage.Get(), navPage, tabs, activeProject, themeMode, closeBehavior)
                .Key(navPage.Get() * 100000 + activeProject.Get())
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(gap),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
        // 底部全局状态条（所有页面共享）：与顶级侧边栏一样无岛屿包裹——
        // 无 Background、无圆角，仅 1pt 分隔线 + 一行小字直接落在窗口背景上。
        // 内容：项目名 / 环境名 / HTTP 引擎 / k6 状态，小字（Label + kCaption，
        // on_surface_variant）。外层 Column 交叉轴 Stretch，分隔线才能拉满整宽。
        huxerui::Column {
            huxerui::Row{}.With(huxerui::Frame{.height = 1.0F},
                                huxerui::Background(rootSpec.colors.outline)),
            huxerui::Row {
                statusText(statusProject),
                statusText(statusEnv),
                statusText("HTTP: curl"),
                statusText(statusK6),
            }
                .With(huxerui::Spacing(rootSpec.spacing.medium),
                      // 左右留白与侧栏/页面边距对齐（spacing.medium）
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                          rootSpec.spacing.medium, 0.0F)),
                      huxerui::Frame{.height = 22.0F},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }
            .With(huxerui::Padding(
                      // 补回根 Column 收窄的间隙：主行↔状态条保持 gap 不变。
                      huxerui::EdgeInsets{statusTopPad, 0.0F, 0.0F, 0.0F}),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
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
