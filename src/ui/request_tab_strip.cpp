// request_tab_strip.cpp — 右岛顶部内部标签条（草稿 chip + 环境选择 + ☰）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 标签条），纯搬移。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
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

// 右岛顶部内部标签条：每个打开的草稿一个标签（点击切换 / ✕ 关闭），末尾 "＋" 新建；
// 最右侧为环境选择 + ☰ 合并控件（"无" + 当前项目环境，选中 = currentEnvId；☰ 开
// 环境配置弹窗）。envVersion 由 RequestPage 持有：环境 CRUD 后 bump，本条按它重读 store。
[[huxerui::composable]] huxerui::View RequestTabStrip(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<int> envVersion, huxerui::State<bool> newTabOpen) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    auto envEditing = huxerui::UseState(false);
    (void)envVersion.Get(); // 订阅环境版本：环境增删改/切换后重组本条
    const auto chipFont = huxerui::Font::System(font_size::kChip);
    const auto badgeFont =
        huxerui::Font::Monospace(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold);

    const std::vector<RequestDraft> snapshot = drafts.Get();
    // 悬停标签 uid：只有悬停的 chip 显示 ✕（0 = 无）。Hover 事件是包含
    // 生命周期：指针进入 chip 呈现边界发 Enter、离开才发 Leave，在子组件
    // （徽标/名称/✕）之间移动不重触发——挂在 chip 最外层即覆盖整 chip。
    auto hoveredChip = huxerui::UseState<std::uint64_t>(0);
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
    // 固定 chip 宽度：拖拽换位/边缘钳制需要已知步进，同 Chrome 固定宽标签。
    // 步进 = chip 宽 + 分隔竖线(1pt) + 两侧间距（竖线作为 Row 子节点占布局，
    // 用 Opacity 显隐避免悬停时回流抖动）。
    constexpr float kChipDragWidth = 160.0F;
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
                          // 限宽给行尾 ✕ 留位（固定宽 160：徽标+名称+✕）。
                          huxerui::Frame{.max_width = 100.0F},
                          huxerui::Indication{})
                    .OnClick([drafts, activeTab, newTabOpen, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) {
                            activeTab = i;
                            newTabOpen = false;
                        }
                    }),
                // 弹性占位把 ✕ 顶到固定宽 chip 的右缘（Spacer 自带 Grow(1)）。
                huxerui::Spacer{},
                // 关闭钮：常驻、透明占位，悬停才显示（Opacity 只改绘制不动结构，
                // 避免悬停重组换子节点类型引起抖动）。透明时点击空转。
                AppIconButton("✕", "关闭请求标签", [tasks, drafts, activeTab, i] {
                        // 关闭会卸载本 ✕ 所在标签：推迟出指针事件路径
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
                // 悬停显隐 ✕：Enter 记 uid，Leave 时仅当仍是本 chip 才清空
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
                huxerui::Text("+", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = badgeFont,
                                              .foreground = theme.colors.on_surface_variant}),
                huxerui::Text("新建请求", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont,
                                              .foreground = theme.colors.on_surface}),
                huxerui::Spacer{},
                AppIconButton("✕", "关闭新建请求标签", [tasks, newTabOpen] {
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
    chips.push_back(AppIconButton("+", "新建请求标签", [newTabOpen] {
                            newTabOpen = true;
                        }, AppIconButtonShape::Circular));

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
                              huxerui::Frame{.max_width = 100.0F}),
                    // 与本体一致：✕ 顶到右缘。
                    huxerui::Spacer{},
                    huxerui::Text("✕", huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{.font = chipFont,
                                                  .foreground = overlayForeground})
                        .With(huxerui::Padding(4.0F)),
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

    // 环境下拉：选项 = "无" + 当前项目环境（组合期按 envVersion 重读 store）。
    const std::vector<db::Environment>& envs = g_requests.environments();
    std::vector<std::string> envNames;
    envNames.reserve(envs.size() + 1);
    envNames.push_back("无");
    std::size_t currentEnv = 0;
    for (const db::Environment& e : envs) {
        if (e.id == g_requests.currentEnvId()) currentEnv = envNames.size();
        envNames.push_back(e.name.empty() ? "（未命名）" : e.name);
    }

    // 环境选择 + ☰ 合并控件。HuxerUI 2659a55 起 ComboBox 原生提供尾图标
    // 和展开状态事件：展开=进搜索态并清空查询，收起=回闭合态。闭合态显示
    // 在组合期从 store 现值派生，保证默认始终是当前环境名，不再自行拼
    // TextField + Popup。
    const IslandTheme islands = ResolveIslandTheme(theme);
    struct EnvChoice {
        std::int64_t id = 0;
        std::string label;
    };
    std::vector<EnvChoice> allChoices{{.id = 0, .label = "无"}};
    for (const db::Environment& env : envs)
        allChoices.push_back(EnvChoice{.id = env.id,
                                       .label = env.name.empty() ? "（未命名）" : env.name});

    // envQuery 只承载搜索态的查询文本；闭合态显示值在组合期从 store 现值
    // 派生（不依赖 State 快照回写——快照漏一次写回就空白/陈旧，默认必须
    // 始终显示当前环境名）。
    auto envQuery = huxerui::UseState(huxerui::TextEditingValue::FromText(""));
    const huxerui::TextEditingValue envValue =
        envEditing.Get()
            ? envQuery.Get()
            : huxerui::TextEditingValue::FromText(envNames.at(currentEnv));
    // 统一切环境出口：菜单点击/键盘 Enter 唯一命中/提交都走这里，失败只弹 toast
    // 不改搜索态。成功则退出搜索态并 bump envVersion 重组（显示随即跟上新值）。
    auto applyEnvChoice = [envEditing, envQuery, envVersion, toast](std::int64_t id) {
        if (const std::string err = g_requests.selectEnv(id); !err.empty()) {
            toast.Show("切换环境失败: " + err);
            return;
        }
        envQuery = huxerui::TextEditingValue::FromText("");
        envEditing = false;
        envVersion = envVersion.Get() + 1;
    };
    std::vector<std::string> filteredEnvironments;
    std::vector<std::int64_t> filteredEnvironmentIds;
    const std::string query = envEditing.Get() ? envQuery.Get().text : std::string{};
    for (const EnvChoice& choice : allChoices) {
        std::string candidate = choice.label;
        std::string needle = query;
        std::ranges::transform(candidate, candidate.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::ranges::transform(needle, needle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (needle.empty() || candidate.find(needle) != std::string::npos) {
            filteredEnvironments.push_back(choice.label);
            filteredEnvironmentIds.push_back(choice.id);
        }
    }
    huxerui::TextFieldStyle envFieldStyle =
        huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    // 顶部标签条自身使用弱前景色；ComboBox 的内部 TextField 不能依赖祖先
    // Foreground 继承，否则受控值已更新但编辑文字、光标会与岛面近乎同色。
    envFieldStyle.text_style.foreground = theme.colors.on_surface;
    envFieldStyle.placeholder_style.foreground = theme.colors.on_surface_variant;
    envFieldStyle.label_style.foreground = theme.colors.on_surface_variant;
    envFieldStyle.floating_label_style.foreground = theme.colors.on_surface_variant;
    envFieldStyle.caret = theme.colors.primary;
    envFieldStyle.composition = theme.colors.primary;
    envFieldStyle.selection = huxerui::Color{
        theme.colors.primary.red, theme.colors.primary.green,
        theme.colors.primary.blue, 0.24F};
    envFieldStyle.trailing_icon = theme.colors.on_surface_variant;
    envFieldStyle.focused_trailing_icon = theme.colors.primary;

    huxerui::View envTrigger = huxerui::ProvideEnvironment(
        envFieldStyle,
        huxerui::View{
        huxerui::ComboBox(envValue, filteredEnvironments)
            .Placeholder(envEditing.Get() ? envNames.at(currentEnv) : "搜索环境")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .TrailingIcon(envEditing.Get() ? app::images::search : app::images::chevron_down)
            // ComboBox 在候选为空且没有 EmptyContent 时会主动关闭。搜索首字
            // 暂无匹配是正常中间态，必须保留弹层和原生输入会话。
            .EmptyContent([] {
                return huxerui::Text("没有匹配的环境", huxerui::TextRole::Label)
                    .With(huxerui::Padding(10.0F));
            })
            .OnExpandedChanged([envEditing, envQuery](bool expanded) {
                if (expanded) {
                    envEditing = true;
                    envQuery = huxerui::TextEditingValue::FromText("");
                    return;
                }
                // 无选中关闭（Escape / 点外部 / 失焦 / 卸载）同样发收起事件：
                // 退出搜索态即可，闭合显示值由组合期从 store 现值派生，无需回写。
                envEditing = false;
            })
            .OnChanged([envEditing, envQuery](const huxerui::TextEditingValue& value) {
                envEditing = true;
                envQuery = value;
            })
            .OnSelected([applyEnvChoice, filteredEnvironmentIds](
                            std::size_t index, const huxerui::TextEditingValue&) {
                if (index >= filteredEnvironmentIds.size()) return;
                applyEnvChoice(filteredEnvironmentIds[index]);
            })
            // 无高亮项时按 Enter 框架发 Submitted（先收起再发）。搜索场景里
            // "输查询词 + Enter"是自然完成手势：唯一命中直接采用；多命中/
            // 无命中回到闭合态（显示仍为当前环境）。
            .OnSubmitted([applyEnvChoice, query, filteredEnvironments,
                          filteredEnvironmentIds] {
                if (query.empty() || filteredEnvironments.size() != 1) return;
                applyEnvChoice(filteredEnvironmentIds[0]);
            })
            .With(huxerui::Frame{.width = 136.0F, .height = islands.control_height},
                  huxerui::ClipChildren())});

    // ☰：环境配置弹窗（自定义内容层，DialogFactory）。P1-A4 收口：原
    // Text("☰")+Padding 热区不足 28pt 且无语义标签/Tooltip，迁为统一 Bare
    // AppIconButton——semanticLabel"环境配置"兼作可访问名称与 Tooltip，hover/
    // press indication 覆盖整个 28×28 命中区。外包垂直居中容器：外层 Row 交叉
    // 轴 Stretch 会把固定高子项拉到行高，包一层 CrossAlign(Center) 保住
    // 28×28 命中区与方形 hover 底。
    huxerui::View envSettingsTrigger =
        huxerui::Row {
            AppIconButton("☰", "环境配置",
                          [dialog, envVersion] {
                              dialog.Show(
                                  [envVersion](huxerui::DialogContext ctx) -> huxerui::View {
                                      return EnvironmentDialog(ctx, envVersion);
                                  },
                                  huxerui::DialogOptions{});
                          },
                          AppIconButtonShape::Bare, 28.0F),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

    return huxerui::Row {
        // 标签 chips 占满剩余宽度（Grow 把环境区推到最右），溢出裁剪。
        // Stack 包裹：拖动时覆盖层克隆叠在 chips 之上（绘制最上层）。
        huxerui::Stack {
            huxerui::Row(std::move(chips))
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            std::move(overlayChip),
        }
            .With(huxerui::ClipChildren(), huxerui::Grow(1.0F)),
        huxerui::Row {
            std::move(envTrigger),
            // 竖分隔线：父 Row 交叉轴 Stretch 拉满全高。
            huxerui::Column{}.With(huxerui::Frame{.width = 1.0F},
                                   huxerui::Background(theme.colors.outline)),
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
