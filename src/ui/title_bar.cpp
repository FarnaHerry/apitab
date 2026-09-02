// title_bar.cpp — 标题栏（Logo + 顶级标签条 TopTabStrip + 拖拽换位，P1-C2 自 app.cpp 纯搬移）：
//   LogoBadge（apitab_mark 矢量徽标）+ TopTab（单个项目/主页/设置标签，激活态/悬停/关闭 ✕）+
//   TopTabStrip（主页钉最左、项目标签横向滚动、设置单例标签固定队尾、分隔竖线、拖拽换位与
//   让位滑动、覆盖层克隆）。命中区规则见 island-structure-theme.md §15：Logo 区与标签条
//   空白为拖动区、标签本体/✕/闪电/齿轮为交互区、弹性 Grow(1) 空白为 WindowDragRegion。
//   本文件仅承载标题栏职责，不含侧栏/内容区/状态条/对话框（见 app.cpp / global_status_bar.cpp / app_dialogs.cpp）。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "ui.h"
#include "app_resources.h"

import apitab.config;
import apitab.db;
import apitab.preferences;
import apitab.store.requests;
import apitab.store.loadtest;
import apitab.utils;

namespace apitab::ui {

namespace {

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

// 顶级标签拖拽载荷：按项目 id 定位源/目标标签。只接受项目——主页标签（固定最左）
// 与设置单例标签（不参与排序、不写入 open_projects，§13.4 B0.1）不挂 DragSource。
struct ProjectTabDragPayload {
    std::int64_t projectId = 0;
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

} // namespace

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

} // namespace apitab::ui
