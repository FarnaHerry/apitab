// request_tab_strip.cpp — 右岛顶部内部标签条（草稿 chip + 环境选择 + 菜单图标）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 标签条），纯搬移。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "ui.h"
#include "draft.h"
#include "app_resources.h"

import apitab.db;
import apitab.store.requests;

namespace apitab::ui {

// 内部标签拖拽载荷：按 uid 定位源/目标标签，与下标无关。
struct DraftTabDragPayload {
    std::uint64_t uid = 0;
};

// 标签页选择弹层（"⌄"下拉）：顶部搜索框 + 过滤后的标签列表，行样式与 chips 对齐
// （方法徽标按 MethodColor 着色 + 草稿名），当前标签高亮。选中后关层并切换标签：
// 关层会卸载被点的行节点，所以 activeTab 的写入必须推迟出指针事件路径（约定 6）；
// 任务派给标签条自己的 TaskScope —— 弹层作用域随关层销毁，不能用它。
[[huxerui::composable]] huxerui::View TabPickerContent(
    huxerui::PopupContext ctx, huxerui::State<std::vector<RequestDraft>> drafts,
    huxerui::State<std::size_t> activeTab, huxerui::State<bool> newTabOpen,
    huxerui::TaskScope tasks) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::MenuStyle menuStyle = huxerui::UseEnvironment<huxerui::MenuStyle>();
    const huxerui::Font badgeFont =
        huxerui::Font::Monospace(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold);
    auto query = huxerui::UseState(huxerui::TextEditingValue::FromText(""));
    const auto lower = [](std::string text) {
        std::ranges::transform(text, text.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string needle = lower(query.Get().text);
    const auto matches = [&needle, &lower](const std::string& text) {
        return needle.empty() || lower(text).find(needle) != std::string::npos;
    };

    const std::vector<RequestDraft> all = drafts.Get();
    const std::size_t activeIndex =
        all.empty() ? 0 : std::min(activeTab.Get(), all.size() - 1);
    std::vector<huxerui::View> rows;
    rows.reserve(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::string badge = DraftKindBadge(all[i]);
        const std::string name = DraftDisplayName(all[i]);
        if (!matches(name) && !matches(badge)) continue;
        const bool active = !newTabOpen.Get() && i == activeIndex;
        rows.push_back(
            huxerui::Row {
                huxerui::Text(badge, huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = badgeFont,
                                              .foreground = MethodColor(theme, badge)})
                    .With(huxerui::Frame{.min_width = 52.0F}),
                huxerui::Text(name, huxerui::TextRole::Body)
                    .With(huxerui::Grow(1.0F), huxerui::ClipChildren()),
                // 行内关闭动作：与 chips 上的 ✕ 同一行为（关掉这个草稿；关的是当前
                // 标签则顺延到相邻标签，关的是前面的标签则当前标签下标跟着前移）。
                // 删除会卸载本行 → 推迟出指针事件路径；弹层不关，可连续关多个。
                AppIconButton(app::images::close, "关闭标签页",
                              [tasks, drafts, activeTab, uid = all[i].uid] {
                                  tasks.Launch([drafts, activeTab, uid]() -> huxerui::Task<void> {
                                      co_await huxerui::Delay(std::chrono::duration<double>{0});
                                      std::vector<RequestDraft> copy = drafts.Get();
                                      for (std::size_t k = 0; k < copy.size(); ++k) {
                                          if (copy[k].uid != uid) continue;
                                          const std::size_t active = activeTab.Get();
                                          copy.erase(copy.begin() + static_cast<long>(k));
                                          drafts = copy;
                                          if (copy.empty())
                                              activeTab = 0;
                                          else if (k == active)
                                              activeTab = std::min(k, copy.size() - 1);
                                          else if (k < active)
                                              activeTab = active - 1;
                                          break;
                                      }
                                  });
                              },
                              AppIconButtonShape::Bare, 24.0F)
                    // 行内 ✕ 不进焦点序：弹层里 12 行就是 12 个焦点停靠点，
                    // 键盘用户用行本身切换即可，关闭动作仍可用鼠标点。
                    .With(huxerui::Focusable(false)),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::Background(active ? theme.colors.surface_container_highest
                                                 : huxerui::Color::Transparent()),
                      huxerui::CornerRadius(menuStyle.corner_radii.top_left / 2.0F),
                      huxerui::Padding(menuStyle.item_padding),
                      huxerui::Frame{.min_height = menuStyle.minimum_item_height},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      menuStyle.item_indication)
                .OnClick([ctx, tasks, drafts, activeTab, newTabOpen, uid = all[i].uid] {
                    ctx.Dismiss(); // 关层会卸载本行：activeTab 写入推迟出指针事件路径
                    tasks.Launch([drafts, activeTab, newTabOpen, uid]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        const std::vector<RequestDraft> now = drafts.Get();
                        for (std::size_t k = 0; k < now.size(); ++k) {
                            if (now[k].uid != uid) continue;
                            activeTab = k;
                            newTabOpen = false;
                            break;
                        }
                    });
                })
                .Key(static_cast<std::int64_t>(all[i].uid)));
    }
    huxerui::View list =
        rows.empty()
            ? huxerui::View{huxerui::Text("没有匹配的标签页", huxerui::TextRole::Label)
                                .With(huxerui::Foreground(theme.colors.on_surface_variant),
                                      huxerui::Padding(10.0F))}
            : huxerui::View{huxerui::ScrollView{
                  huxerui::Column(std::move(rows)).With(
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
                                 .With(huxerui::ScrollBar(),
                                       huxerui::Frame{.max_height = 300.0F})};
    // 固定宽：弹层测量上限≈视口宽，行内 Grow 会把宽度顶到上限（见 CLAUDE.md
    // "行宽自适应"），所以这里给内容一个明确的宽度上界。
    return huxerui::Column {
        huxerui::TextField(query)
            .Placeholder("搜索标签页")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([query](const huxerui::TextEditingValue& value) { query = value; }),
        std::move(list),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = 260.0F},
              huxerui::Background(menuStyle.background),
              huxerui::CornerRadius(menuStyle.corner_radii.top_left),
              huxerui::Padding(menuStyle.content_padding),
              huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 标签条溢出入口：内容宽度超出视口时才出现的"无尾下箭头"。只在本作用域读
// ScrollController 的度量（Metrics() 会订阅该状态）——滚动时 offset 变化只重组
// 这个小作用域，不会牵动整条标签条；点开是带搜索的标签页选择弹层。
// 标签选择入口："无尾下箭头" + 可搜索标签弹层。常驻：与"＋"一起跟在最后一个
// 标签后面，标签放不下时整个动作组被 TabTrailingProbe 移到行右缘固定。
[[huxerui::composable]] huxerui::View TabOverflowButton(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<bool> newTabOpen, huxerui::TaskScope tasks) {
    auto popup = huxerui::UsePopup();
    return AppIconButton(
               app::images::chevron_down, "全部标签页",
               [popup, drafts, activeTab, newTabOpen, tasks] {
                   popup.Show(
                       [drafts, activeTab, newTabOpen, tasks](huxerui::PopupContext ctx) {
                           return TabPickerContent(ctx, drafts, activeTab, newTabOpen, tasks);
                       },
                       huxerui::PopupOptions{.placement = {huxerui::AnchorSide::Below,
                                                           huxerui::AnchorAlignment::End}});
               },
               AppIconButtonShape::Bare)
        .With(popup.Anchor());
}

// 末尾动作组（＋ / ⌄）落位探针：读 ScrollController 度量判断"标签 + 动作组"
// 是否放得下——放不下就在可滚区外渲染动作组（＝固定在行右缘），否则渲染空占位
// （动作组已经内联在 chips 末尾，视觉上跟着最后一个标签）。判定结果回写 pinned。
// 判定式在两种落位下都自洽：内联时 content 含动作组、viewport 是整条；固定时
// content 不含、viewport 已扣掉动作组——`content > viewport` 恒等于"放不下"，
// 切换落位不会来回抖。写入推迟出组合期（State 等值写入本身是 no-op）。
[[huxerui::composable]] huxerui::View TabTrailingProbe(
    huxerui::ScrollController controller, huxerui::State<bool> pinned,
    huxerui::View group) {
    const huxerui::ScrollMetrics metrics = controller.Metrics();
    const bool overflow = metrics.content_extent > metrics.viewport_extent;
    // 回写落位必须在组合期之外：Lifecycle 在帧提交后运行，且只在 overflow 变化时
    // 重跑（State 等值写入本身也是 no-op）。
    huxerui::Lifecycle([pinned, overflow] { pinned = overflow; }, overflow);
    return pinned.Get() ? std::move(group) : huxerui::View{huxerui::Row{}};
}

// 悬停滚动指示条：内容溢出且悬停标签条时，在标签行底部画一条自绘横向滚动条。
// 框架 ScrollBar 只在滚动活动时淡入、空闲即隐（hover 看不见），这里显式跟随
// hover。它是可滚内容里的 paint-only 覆盖层（Offset 只平移绘制），所以 x 要加上
// 当前 offset 才是相对视口的位置；滚动时只重组本作用域，不牵动整条标签条。
[[huxerui::composable]] huxerui::View TabScrollIndicator(huxerui::ScrollController controller,
                                                          huxerui::State<bool> hovered) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::ScrollMetrics metrics = controller.Metrics();
    if (!hovered.Get() || metrics.content_extent <= metrics.viewport_extent + 1.0F ||
        metrics.content_extent <= 0.0F)
        return huxerui::Row{};
    const float track = std::max(0.0F, metrics.viewport_extent - 4.0F);
    const float bar = std::clamp(metrics.viewport_extent * metrics.viewport_extent /
                                     metrics.content_extent,
                                 24.0F, track);
    const float progress = metrics.maximum_offset > 0.0F
                               ? std::clamp(metrics.offset / metrics.maximum_offset, 0.0F, 1.0F)
                               : 0.0F;
    huxerui::Color thumb = theme.colors.on_surface_variant;
    thumb.alpha = 0.55F;
    return huxerui::Row{}.With(
        huxerui::Frame{.width = bar, .height = 3.0F},
        huxerui::Background(thumb), huxerui::CornerRadius(theme.shapes.full),
        huxerui::Offset(huxerui::Point{
            metrics.offset + 2.0F + (track - bar) * progress, 23.0F}));
}

// 右岛顶部内部标签条：每个打开的草稿一个标签（点击切换 / 关闭图标），末尾加号图标新建；
// 最右侧为环境选择 + 菜单图标合并控件（"无" + 当前项目环境，选中 = currentEnvId；菜单图标打开
// 环境配置弹窗）。envVersion 由 RequestPage 持有：环境 CRUD 后 bump，本条按它重读 store。
[[huxerui::composable]] huxerui::View RequestTabStrip(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<int> envVersion, huxerui::State<bool> newTabOpen) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    (void)envVersion.Get(); // 订阅环境版本：环境增删改/切换后重组本条
    const auto chipFont = huxerui::Font::System(font_size::kChip);
    const auto badgeFont =
        huxerui::Font::Monospace(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold);

    const std::vector<RequestDraft> snapshot = drafts.Get();
    // 悬停标签 uid：只有悬停的 chip 显示关闭图标（0 = 无）。Hover 事件是包含
    // 生命周期：指针进入 chip 呈现边界发 Enter、离开才发 Leave，在子组件
    // （徽标/名称/关闭图标）之间移动不重触发——挂在 chip 最外层即覆盖整 chip。
    auto hoveredChip = huxerui::UseState<std::uint64_t>(0);
    auto newTabHovered = huxerui::UseState(false);
    // 拖拽中的水平位移（Chrome 式贴条滑动，同顶级标签）：被拖 chip 的 uid +
    // X 位移（已按条内容范围钳制）。被拖 chip 本体变透明占位，视觉由条内
    // 覆盖层克隆接管（见下方 overlayChip）——覆盖层无任何事件 handler，
    // 命中测试穿透到下方静止 chip，drop 才能落到邻居上（否则被拖 chip 的
    // 偏移体永远顶在指针下，吞掉 drop）。
    auto dragUid = huxerui::UseState<std::uint64_t>(0);
    auto dragDx = huxerui::UseState(0.0F);
    // 拖动起点的槽位下标（Started 时记录）：覆盖层 X = 起点槽位 + 累计位移，
    // 与实时换位后的数据下标解耦，视觉连续不跳变。
    auto dragOrig = huxerui::UseState(0);
    // 让位滑动（StartSlide/SlideCell 见 ui.h）：tick 仅作重组触发器。
    auto slideCell = huxerui::UseState(std::make_shared<SlideCell>());
    auto slideTick = huxerui::UseState<std::uint64_t>(0);
    (void)slideTick.Get(); // 订阅：tween 每步 bump 触发重组
    // 标签条横向滚动控制器：ScrollView 用它做溢出滚动；TabTrailingProbe 用它判定
    // "标签 + 末尾动作组"是否放得下，TabScrollIndicator 用它画悬停滚动条。
    auto tabsScroll = huxerui::UseScrollController();
    // 悬停整条标签区（滚动指示条只在 hover 时显示）。
    auto tabsHovered = huxerui::UseState(false);
    // 末尾动作组（＋ / ⌄）的落位：false = 内联在最后一个标签之后；true = 溢出，
    // 固定在可滚区右侧。由 TabTrailingProbe 按度量回写。
    auto trailingPinned = huxerui::UseState(false);
    // 固定 chip 宽度：拖拽换位/边缘钳制需要已知步进，同 Chrome 固定宽标签。
    // 步进 = chip 宽 + 分隔竖线(1pt) + 两侧间距（竖线作为 Row 子节点占布局，
    // 用 Opacity 显隐避免悬停时回流抖动）。
    // 标签宽度：从 160 收到 140，同宽窗口能多放下的标签数（名称限宽同步收窄）。
    constexpr float kChipDragWidth = 140.0F;
    const float chipStride = kChipDragWidth + 1.0F + 2.0F * theme.spacing.small;
    // 标签间分隔竖线：始终占布局（Opacity 显隐），相邻标签激活/悬停/被拖时
    // 隐藏；高度小于行高，上下留空隙不连通。
    auto chipDivider = [&](bool visible) {
        return huxerui::View{
            huxerui::Column{}.With(huxerui::Frame{.width = 1.0F, .height = 14.0F},
                                   huxerui::Background(theme.colors.outline),
                                   huxerui::Opacity(visible ? 1.0F : 0.0F))};
    };
    // 实时换位（Chrome 式：拖过邻居槽位即交换，不等松手）：把 uid 移动到
    // 目标槽位，activeTab 按 uid 跟随；被挤动的邻居加让位滑动。
    // keyed 重排不卸载节点，同步写即可。
    auto moveDraftTo = [drafts, activeTab, tasks, slideCell, slideTick,
                        chipStride](std::uint64_t uid, std::size_t desired) {
        std::vector<RequestDraft> copy = drafts.Get();
        auto findUid = [&copy](std::uint64_t u) {
            for (std::size_t k = 0; k < copy.size(); ++k)
                if (copy[k].uid == u) return k;
            return copy.size();
        };
        const std::size_t from = findUid(uid);
        if (from >= copy.size() || desired >= copy.size() || from == desired) return;
        // 让位滑动：from<desired 时 (from,desired] 的邻居左移一格（残量
        // +stride），反之 [desired,from) 右移一格（残量 -stride）。
        if (from < desired) {
            for (std::size_t k = from + 1; k <= desired; ++k)
                StartSlide(tasks, slideCell, slideTick,
                           static_cast<std::int64_t>(copy[k].uid), chipStride);
        } else {
            for (std::size_t k = desired; k < from; ++k)
                StartSlide(tasks, slideCell, slideTick,
                           static_cast<std::int64_t>(copy[k].uid), -chipStride);
        }
        const std::uint64_t activeUid =
            activeTab.Get() < copy.size() ? copy[activeTab.Get()].uid : 0;
        RequestDraft moved = std::move(copy[from]);
        copy.erase(copy.begin() + static_cast<long>(from));
        copy.insert(copy.begin() + static_cast<long>(desired), std::move(moved));
        for (std::size_t k = 0; k < copy.size(); ++k)
            if (copy[k].uid == activeUid) activeTab = k;
        drafts = copy;
    };
    std::vector<huxerui::View> chips;
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const bool active = !newTabOpen.Get() && i == activeTab.Get();
        const bool chipHovered = hoveredChip.Get() == snapshot[i].uid;
        // 背景默认不显示（透明）：激活 = 最高层级容器底，悬停 = 略深容器底，
        // 常态下只靠竖线分隔标签。
        const huxerui::Color fill =
            active ? theme.colors.surface_container_highest
                   : (chipHovered ? theme.colors.surface_container
                                  : huxerui::Color::Transparent());
        const huxerui::Color foreground =
            active ? theme.colors.on_surface : theme.colors.on_surface_variant;
        const std::string chipBadge = DraftKindBadge(snapshot[i]);
        chips.push_back(
            huxerui::Row {
                // 类型徽标：HTTP 显示方法名，WS/TCP 显示类型缩写。显式空
                // Indication：整 chip 的悬停反馈由外层 fill 承担，压掉内层默认高亮。
                // 徽标按 MethodColor 统一色表逐方法着色。
                huxerui::Text(chipBadge, huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{
                        .font = badgeFont,
                        .foreground = MethodColor(theme, chipBadge)})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(2.0F, 2.0F)),
                          huxerui::Indication{})
                    .OnClick([drafts, activeTab, newTabOpen, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) {
                            activeTab = i;
                            newTabOpen = false;
                        }
                    }),
                huxerui::Text(DraftDisplayName(snapshot[i]), huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont, .foreground = foreground})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                          // 限宽给行尾关闭图标留位（固定宽 160：徽标+名称+关闭图标）。
                          huxerui::Frame{.max_width = 80.0F},
                          huxerui::Indication{})
                    .OnClick([drafts, activeTab, newTabOpen, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) {
                            activeTab = i;
                            newTabOpen = false;
                        }
                    }),
                // 弹性占位把关闭图标顶到固定宽 chip 的右缘（Spacer 自带 Grow(1)）。
                huxerui::Spacer{},
                // 关闭钮：常驻、透明占位，悬停才显示（Opacity 只改绘制不动结构，
                // 避免悬停重组换子节点类型引起抖动）。透明时点击空转。
                AppIconButton(app::images::close, "关闭请求标签", [tasks, drafts, activeTab, i] {
                        // 关闭会卸载本按钮所在标签：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<RequestDraft> copy = drafts.Get();
                            if (i >= copy.size()) co_return;
                            copy.erase(copy.begin() + static_cast<long>(i));
                            drafts = copy;
                            if (!copy.empty() && activeTab.Get() >= copy.size())
                                activeTab = copy.size() - 1;
                        });
                    }, AppIconButtonShape::Bare, 28.0F, false, chipHovered)
                    .With(huxerui::Opacity(chipHovered ? 1.0F : 0.0F)),
            }
                .With(huxerui::Spacing(0.0F), huxerui::Background(fill),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                      huxerui::Frame{.width = kChipDragWidth, .height = 28.0F},
                      huxerui::ClipChildren(),
                      // 拖动时本体变透明占位：保留布局槽位与拖拽会话，
                      // 视觉由覆盖层克隆接管。
                      huxerui::Opacity(dragUid.Get() == snapshot[i].uid ? 0.0F : 1.0F),
                      // 让位滑动残量（非拖动标签恒 0）：实时换位时邻居从旧
                      // 槽位滑入新槽位。
                      huxerui::Offset(huxerui::Point{
                          SlideOffsetOf(slideCell.Get(),
                                        static_cast<std::int64_t>(snapshot[i].uid)),
                          0.0F}),
                      // 标签拖拽换位：限水平轴（axis=Horizontal，竖向移动不进入
                      // 拖拽），与内层徽标/文字的点击切换按阈值分胜负；无悬浮
                      // 拖影。请求级标签不支持拖出成独立窗口。
                      huxerui::DragSource(
                          DraftTabDragPayload{snapshot[i].uid},
                          huxerui::DragGesture{.axis = huxerui::Axis::Horizontal}))
                // 悬停显隐关闭图标：Enter 记 uid，Leave 时仅当仍是本 chip 才清空
                // （防跨 chip 误清）。只写 hoveredChip，不做重活。
                .On<huxerui::ViewEvents::Hover>(
                    [hoveredChip, uid = snapshot[i].uid](const huxerui::HoverEvent& e) {
                        if (e.type == huxerui::HoverEventType::Enter)
                            hoveredChip = uid;
                        else if (e.type == huxerui::HoverEventType::Leave &&
                                 hoveredChip.Get() == uid)
                            hoveredChip = 0;
                    })
                // 拖拽开始：记录被拖 chip 与起点槽位。
                .On<huxerui::DragSourceEvents::Started>(
                    [dragUid, dragOrig, i, uid = snapshot[i].uid](
                        const huxerui::DragEvent&) {
                        dragUid = uid;
                        dragOrig = static_cast<int>(i);
                    })
                // 拖动中每帧：钳制后的累计 X 位移写入 dragDx（驱动覆盖层），
                // 并按"经过即换位"实时移动数据顺序——目标槽位 = 起点 +
                // round(位移/步进)。钳制范围 = 起点槽位到条内容两端，拖到
                // 容器外贴边停住。keyed 重排不卸载节点，同步写即可。
                .On<huxerui::DragSourceEvents::Changed>(
                    [dragUid, dragDx, dragOrig, chipStride, n = snapshot.size(),
                     moveDraftTo, uid = snapshot[i].uid](const huxerui::DragEvent& e) {
                        dragUid = uid;
                        const float orig = static_cast<float>(dragOrig.Get());
                        const float lo = -orig * chipStride;
                        const float hi =
                            static_cast<float>(n > 0 ? n - 1 : 0) * chipStride - orig * chipStride;
                        const float t = std::clamp(e.translation.x, lo, hi);
                        dragDx = t;
                        long desired =
                            static_cast<long>(orig) + std::lround(t / chipStride);
                        desired = std::clamp<long>(desired, 0,
                                                   static_cast<long>(n > 0 ? n - 1 : 0));
                        moveDraftTo(uid, static_cast<std::size_t>(desired));
                    })
                // 结束/取消：归零会移除覆盖层节点（卸载），推迟出指针事件路径。
                .On<huxerui::DragSourceEvents::Ended>(
                    [tasks, dragUid, dragDx](const huxerui::DragDropResult&) {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            dragUid = 0;
                            dragDx = 0.0F;
                        });
                    })
                .On<huxerui::DragSourceEvents::Canceled>(
                    [tasks, dragUid, dragDx](const huxerui::DragEvent&) {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            dragUid = 0;
                            dragDx = 0.0F;
                        });
                    })
                // Key 用稳定 uid：未保存草稿 savedId 恒为 0，不能再用下标兜底。
                .Key(static_cast<std::int64_t>(snapshot[i].uid)));
        // 标签间分隔竖线（最后一个草稿与 "＋" 之间不加）：相邻标签激活/
        // 悬停/被拖时隐藏，但始终占布局（Opacity 显隐，不回流）。
        if (i + 1 < snapshot.size()) {
            const bool sepVisible = !active && i + 1 != activeTab.Get() &&
                                    hoveredChip.Get() != snapshot[i].uid &&
                                    hoveredChip.Get() != snapshot[i + 1].uid &&
                                    dragUid.Get() != snapshot[i].uid &&
                                    dragUid.Get() != snapshot[i + 1].uid;
            chips.push_back(chipDivider(sepVisible));
        }
    }
    // 临时“新建请求”标签：不提前制造 HTTP 草稿；选中类型后才转为正式草稿。
    if (newTabOpen.Get()) {
        if (!snapshot.empty()) chips.push_back(chipDivider(false));
        chips.push_back(
            huxerui::Row {
                huxerui::Image(app::images::add)
                    .Fit(huxerui::ImageFit::Contain)
                    .Tint(theme.colors.on_surface_variant)
                    .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
                huxerui::Text("新建请求", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont,
                                              .foreground = theme.colors.on_surface}),
                huxerui::Spacer{},
                AppIconButton(app::images::close, "关闭新建请求标签", [tasks, newTabOpen] {
                    tasks.Launch([newTabOpen]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        newTabOpen = false;
                    });
                }, AppIconButtonShape::Bare),
            }
                .With(huxerui::Spacing(0.0F),
                      huxerui::Background(theme.colors.surface_container_highest),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                      huxerui::Frame{.width = kChipDragWidth, .height = 28.0F},
                      huxerui::ClipChildren()));
    }
    // 末尾 "＋"：只打开新建状态标签，类型卡片确认后才创建草稿。
    // 常态保持透明且无轮廓，悬停时仅显示圆形轮廓。
    huxerui::View newTabButton = AppIconButton(
        app::images::add, "新建请求标签", [newTabOpen] { newTabOpen = true; },
        AppIconButtonShape::Bare);
    newTabButton = std::move(newTabButton)
                       .With(huxerui::CornerRadius(theme.shapes.full),
                             huxerui::Border(newTabHovered.Get()
                                                 ? theme.colors.outline
                                                 : huxerui::Color::Transparent(),
                                             1.0F))
                       .On<huxerui::ViewEvents::Hover>(
                           [newTabHovered](const huxerui::HoverEvent& e) {
                               newTabHovered = e.type != huxerui::HoverEventType::Leave;
                           });
    // "＋"与"⌄"都常驻，默认**跟在最后一个标签后面**（内联进可滚内容，
    // 视觉上贴着标签列表）；只有"标签 + 动作组"真的放不下时，才移到可滚区外、固定
    // 在行右缘，保证溢出状态下新建与选择入口始终可点。落位见 TabTrailingProbe
    // （按 ScrollController 度量在小子作用域里判定并回写 pinned）。
    const bool pinnedNow = trailingPinned.Get();
    auto buildTrailingGroup = [&, newTabButton]() mutable {
        return huxerui::Row {
            newTabButton,
            TabOverflowButton(drafts, activeTab, newTabOpen, tasks),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    };
    huxerui::View pinnedGroup{};
    if (pinnedNow) {
        pinnedGroup = buildTrailingGroup();
    } else {
        chips.push_back(buildTrailingGroup());
    }

    // 拖拽覆盖层：被拖 chip 的视觉克隆（纯展示，无事件/悬停 handler——命中
    // 测试穿透到下方静止 chip）。Stack 中最后声明 = 绘制最上层（充当
    // z-index）。X = 拖拽起点槽位 + 钳制后的累计位移（与实时换位后的数据
    // 下标解耦，换位不引起视觉跳变）；Y 恒 0。
    huxerui::View overlayChip = huxerui::Row{};
    if (dragUid.Get() != 0) {
        for (std::size_t j = 0; j < snapshot.size(); ++j) {
            if (snapshot[j].uid != dragUid.Get()) continue;
            const bool overlayActive = !newTabOpen.Get() && j == activeTab.Get();
            const huxerui::Color overlayFill =
                overlayActive ? theme.colors.surface_container_highest
                              : theme.colors.surface_container;
            const huxerui::Color overlayForeground =
                overlayActive ? theme.colors.on_surface : theme.colors.on_surface_variant;
            const std::string overlayBadge = DraftKindBadge(snapshot[j]);
            overlayChip =
                huxerui::Row {
                    huxerui::Text(overlayBadge, huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{
                            .font = badgeFont,
                            .foreground = MethodColor(theme, overlayBadge)})
                        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(2.0F, 2.0F))),
                    huxerui::Text(DraftDisplayName(snapshot[j]), huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{.font = chipFont,
                                                  .foreground = overlayForeground})
                        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                              huxerui::Frame{.max_width = 80.0F}),
                    // 与本体一致：关闭图标顶到右缘。
                    huxerui::Spacer{},
                    huxerui::Image(app::images::close)
                        .Fit(huxerui::ImageFit::Contain)
                        .Tint(overlayForeground)
                        .With(huxerui::Frame{.width = 16.0F, .height = 16.0F}),
                }
                    .With(huxerui::Spacing(0.0F), huxerui::Background(overlayFill),
                          huxerui::CornerRadius(theme.shapes.small),
                          huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                          huxerui::Frame{.width = kChipDragWidth, .height = 28.0F},
                          huxerui::ClipChildren(),
                          huxerui::Offset(huxerui::Point{
                              static_cast<float>(dragOrig.Get()) * chipStride + dragDx.Get(),
                              0.0F}));
            break;
        }
    }

    // 环境选择 + 菜单图标合并控件。选项 = "无" + 当前项目环境；搜索、展开/收起和
    // 闭合态选中项显示统一由作者推荐的 SearchablePicker 负责。
    const std::vector<db::Environment>& envs = g_requests.environments();
    const IslandTheme islands = ResolveIslandTheme(theme);
    std::vector<SearchItem> envItems{{.id = "0", .label = "无"}};
    envItems.reserve(envs.size() + 1);
    for (const db::Environment& env : envs)
        envItems.push_back(SearchItem{.id = std::to_string(env.id),
                                      .label = env.name.empty() ? "（未命名）" : env.name});

    auto selectedEnvId = huxerui::UseState<std::optional<std::string>>(
        std::to_string(g_requests.currentEnvId()));
    const std::string storeEnvId = std::to_string(g_requests.currentEnvId());
    if (!selectedEnvId.Get() || *selectedEnvId.Get() != storeEnvId) {
        selectedEnvId = storeEnvId;
    }

    // 统一切环境出口：菜单点击/键盘 Enter 唯一命中都走这里，失败只弹 toast；
    // 成功 bump envVersion，让闭合态标签和 URL 基础地址同步刷新。
    auto applyEnvChoice = [selectedEnvId, envVersion, toast](const std::string& id) {
        std::int64_t envId = 0;
        const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), envId);
        if (error != std::errc{} || end != id.data() + id.size()) {
            toast.Show("切换环境失败: 环境标识无效");
            selectedEnvId = std::to_string(g_requests.currentEnvId());
            return;
        }
        if (const std::string err = g_requests.selectEnv(envId); !err.empty()) {
            toast.Show("切换环境失败: " + err);
            selectedEnvId = std::to_string(g_requests.currentEnvId());
            return;
        }
        envVersion = envVersion.Get() + 1;
    };
    huxerui::TextFieldStyle envFieldStyle =
        huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    // 顶部标签条自身使用弱前景色；ComboBox 的内部 TextField 不能依赖祖先
    // Foreground 继承，否则受控值已更新但编辑文字、光标会与岛面近乎同色。
    envFieldStyle.text_style.foreground = theme.colors.on_surface;
    envFieldStyle.placeholder_style.foreground = theme.colors.on_surface_variant;
    envFieldStyle.label_style.foreground = theme.colors.on_surface_variant;
    envFieldStyle.floating_label_style.foreground = theme.colors.on_surface_variant;
    // 标签条中的环境选择器是 32pt 紧凑控件；Material TextField 默认最小高
    // 度为 56pt、上下 padding 为 16pt，放进固定高度并裁剪的环境区后会把
    // 编辑文字挤出可视区域。这里沿用 URL 工具栏的紧凑尺寸，同时把外框交给
    // envTrigger，避免内部描边覆盖主题边界。
    envFieldStyle.variant = huxerui::TextFieldVariant::Outlined;
    envFieldStyle.outlined.minimum_height = islands.control_height;
    envFieldStyle.outlined.border = huxerui::Color::Transparent();
    envFieldStyle.outlined.hovered_border = huxerui::Color::Transparent();
    envFieldStyle.outlined.focused_border = huxerui::Color::Transparent();
    envFieldStyle.outlined.corner_radii = huxerui::CornerRadii{0.0F};
    envFieldStyle.padding = huxerui::EdgeInsets::Symmetric(8.0F, 6.0F);
    envFieldStyle.caret = theme.colors.primary;
    envFieldStyle.composition = theme.colors.primary;
    envFieldStyle.selection = huxerui::Color{
        theme.colors.primary.red, theme.colors.primary.green,
        theme.colors.primary.blue, 0.24F};
    envFieldStyle.trailing_icon = theme.colors.on_surface_variant;
    envFieldStyle.focused_trailing_icon = theme.colors.primary;

    huxerui::View envPicker = SearchablePicker(envItems, selectedEnvId, app::images::chevron_down,
                                               app::images::search, applyEnvChoice, true);
    huxerui::View envTrigger = huxerui::ProvideEnvironment(
        envFieldStyle,
        std::move(envPicker)
            .With(huxerui::Frame{.width = 136.0F, .height = islands.control_height},
                  huxerui::ClipChildren()));

    // 齿轮图标：环境配置弹窗（自定义内容层，DialogFactory）——配置入口统一用
    // 齿轮识别（与项目设置/全局设置一致）。P1-A4 收口：原
    // Text+Padding 热区不足 28pt 且无语义标签/Tooltip，迁为统一 Bare
    // AppIconButton——semanticLabel"环境配置"兼作可访问名称与 Tooltip，hover/
    // press indication 覆盖整个 28×28 命中区。外包垂直居中容器：外层 Row 交叉
    // 轴 Stretch 会把固定高子项拉到行高，包一层 CrossAlign(Center) 保住
    // 28×28 命中区与方形 hover 底。
    huxerui::View envSettingsTrigger =
        huxerui::Row {
            AppIconButton(app::images::gear, "环境配置",
                          [dialog, envVersion] {
                              dialog.Show(
                                  [envVersion](huxerui::DialogContext ctx) -> huxerui::View {
                                      return EnvironmentDialog(ctx, envVersion).Key(envVersion.Get());
                                  },
                                  huxerui::DialogOptions{});
                          },
                          AppIconButtonShape::Bare, 28.0F),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    return huxerui::Row {
        // 标签 chips 占满剩余宽度（Grow 把环境区推到最右）。打开草稿多到放不下时
        // 不再直接裁掉，而是横向滚动（与标题栏项目标签条同一套做法）。
        // Stack 包裹：拖动覆盖层克隆与悬停滚动指示条叠在 chips 之上（后声明 = 绘制
        // 最上层），并随 ScrollView 一起滚动（Offset 只平移绘制，布局原点仍在内容
        // 坐标系——所以指示条的 x 还要加上当前 offset 才能钉在视口里）。
        huxerui::ScrollView(huxerui::Stack {
            huxerui::Row(std::move(chips))
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            std::move(overlayChip),
            TabScrollIndicator(tabsScroll, tabsHovered),
        })
            .ScrollAxis(huxerui::Axis::Horizontal)
            .Controller(tabsScroll)
            // 竖向滚轮映射成横向滚动（框架只按轴分配 delta_x/delta_y，横向容器
            // 收不到竖向滚轮）：滚轮向下 = 列表向右移动，向上 = 向左。
            .On<huxerui::ViewEvents::ScrollInput>(
                [tabsScroll](const huxerui::ScrollInputEvent& event) {
                    const float delta = event.delta_x != 0.0F ? event.delta_x : -event.delta_y;
                    if (delta == 0.0F) return false;
                    tabsScroll.ScrollBy(delta);
                    return true; // 消费掉：不再穿透到页面垂直滚动
                })
            // 悬停整条标签区时显示自绘横向滚动指示条（框架 ScrollBar 只在滚动时
            // 淡入、空闲即隐，hover 看不见；这里显式跟随 hover）。
            .On<huxerui::ViewEvents::Hover>([tabsHovered](const huxerui::HoverEvent& event) {
                tabsHovered = event.type != huxerui::HoverEventType::Leave;
            })
            .With(huxerui::ClipChildren(), huxerui::Grow(1.0F)),
        TabTrailingProbe(tabsScroll, trailingPinned, std::move(pinnedGroup)),
        huxerui::Row {
            std::move(envTrigger),
            // 竖分隔线：父 Row 交叉轴 Stretch 拉满全高；纯装饰线用半透明档。
            huxerui::Column{}.With(huxerui::Frame{.width = 1.0F},
                                   huxerui::Background(islands.outline_hair)),
            std::move(envSettingsTrigger),
        }
            .With(huxerui::Spacing(0.0F),
                  huxerui::Border(theme.colors.outline, 1.0F),
                  huxerui::CornerRadius(islands.control_radius), huxerui::ClipChildren(),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::ClipChildren());
}

} // namespace apitab::ui
