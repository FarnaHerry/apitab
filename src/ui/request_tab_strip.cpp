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
    huxerui::State<int> envVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    auto envSearching = huxerui::UseState(false);
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
        const bool active = i == activeTab.Get();
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
                    .OnClick([drafts, activeTab, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) activeTab = i;
                    }),
                huxerui::Text(DraftDisplayName(snapshot[i]), huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont, .foreground = foreground})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                          // 限宽给行尾 ✕ 留位（固定宽 160：徽标+名称+✕）。
                          huxerui::Frame{.max_width = 100.0F},
                          huxerui::Indication{})
                    .OnClick([drafts, activeTab, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) activeTab = i;
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
    // 末尾 "＋"：新建草稿标签（不卸载任何节点，同步写即可）。
    chips.push_back(AppIconButton("+", "新建请求标签", [drafts, activeTab] {
                            std::vector<RequestDraft> copy = drafts.Get();
                            copy.push_back(RequestDraft{});
                            drafts = copy;
                            activeTab = copy.size() - 1;
                        }, AppIconButtonShape::Circular));

    // 拖拽覆盖层：被拖 chip 的视觉克隆（纯展示，无事件/悬停 handler——命中
    // 测试穿透到下方静止 chip）。Stack 中最后声明 = 绘制最上层（充当
    // z-index）。X = 拖拽起点槽位 + 钳制后的累计位移（与实时换位后的数据
    // 下标解耦，换位不引起视觉跳变）；Y 恒 0。
    huxerui::View overlayChip = huxerui::Row{};
    if (dragUid.Get() != 0) {
        for (std::size_t j = 0; j < snapshot.size(); ++j) {
            if (snapshot[j].uid != dragUid.Get()) continue;
            const bool overlayActive = j == activeTab.Get();
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

    // 环境选择 + ☰ 合并控件（同 MethodUrlBar 思路）：一个共用描边圆角外框 +
    // Spacing(0)，左侧环境选择扁平触发器（环境名 + 统一无尾箭头弹自绘下拉，选中项
    // 深色填充底色、无对钩；保持自绘而非官方 Select——Select 触发器自带描边
    // 外观，塞不进共用外框），中间 1pt 竖分隔线，右侧 ☰ 为 Bare AppIconButton
    // （环境配置弹窗入口；常态透明、hover 才显统一圆角方形底——与左侧触发器
    // 同一"框内扁平段"视觉）。外层 Row 交叉轴 Stretch 让分隔线拉满全高，
    // 左触发器与右侧按钮各自 CrossAlign(Center) 垂直居中。
    // 外框圆角取几何令牌 control_radius（P1-A3：不再散落 shapes.small 字面量）。
    const IslandTheme islands = ResolveIslandTheme(theme);
    struct EnvChoice {
        std::int64_t id = 0;
        std::string label;
    };
    std::vector<EnvChoice> allChoices{{.id = 0, .label = "无"}};
    for (const db::Environment& env : envs)
        allChoices.push_back(EnvChoice{.id = env.id,
                                       .label = env.name.empty() ? "（未命名）" : env.name});

    // 官方 ComboBox：受控输入只用于搜索过滤，选中建议才真正切换环境。
    // 初始化为当前环境名称；选择后同步回写完整 TextEditingValue。
    auto envQuery = huxerui::UseState(
        huxerui::TextEditingValue{envNames.at(currentEnv)});
    const std::string needle = envQuery.Get().text;
    auto containsFolded = [](const std::string& value, const std::string& query) {
        std::string lhs = value;
        std::string rhs = query;
        std::ranges::transform(lhs, lhs.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::ranges::transform(rhs, rhs.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return rhs.empty() || lhs.find(rhs) != std::string::npos;
    };
    std::vector<EnvChoice> matchingChoices;
    for (const EnvChoice& choice : allChoices)
        if (containsFolded(choice.label, needle)) matchingChoices.push_back(choice);

    huxerui::View envSearch =
        huxerui::ComboBox(
            envQuery, matchingChoices,
            [](const EnvChoice& choice) { return choice.label; },
            [](const EnvChoice& choice) { return huxerui::Text(choice.label); })
            .Placeholder("搜索环境")
            .Variant(huxerui::TextFieldVariant::Standard)
            .EmptyContent([] {
                return huxerui::Text("没有匹配的环境", huxerui::TextRole::Label)
                    .With(huxerui::Padding(10.0F));
            })
            .OnChanged([envQuery](const huxerui::TextEditingValue& value) {
                envQuery = value;
            })
            .OnSelected([tasks, envQuery, envSearching, matchingChoices, envVersion, toast](
                            std::size_t selected, const huxerui::TextEditingValue& value) {
                if (selected >= matchingChoices.size()) return;
                if (const std::string err =
                        g_requests.selectEnv(matchingChoices[selected].id);
                    !err.empty()) {
                    toast.Show("切换环境失败: " + err);
                    return;
                }
                envQuery = value;
                envVersion = envVersion.Get() + 1;
                tasks.Launch([envSearching]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    envSearching = false;
                });
            })
            // 搜索态原位替换紧凑按钮，不能改变标题栏的占位尺寸。
            // 不再用失焦关闭：只有完成选择才恢复为紧凑按钮，避免点击候选项时闪烁。
            .With(huxerui::Frame{.width = 136.0F, .height = islands.control_height},
                  huxerui::ClipChildren());

    huxerui::View envCompact = huxerui::Row {
        huxerui::Row{huxerui::Text(envNames.at(currentEnv), huxerui::TextRole::Body)
                          .With(huxerui::Foreground(theme.colors.on_surface),
                                huxerui::ClipChildren())}
            .With(huxerui::Frame{.width = 112.0F, .height = islands.control_height},
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 0.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row{huxerui::Image(app::images::chevron_down)
                          .Fit(huxerui::ImageFit::Contain)
                          .Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center)
                          .Tint(theme.colors.on_surface_variant)
                          .With(huxerui::Frame{.width = 12.0F, .height = 12.0F})}
            .With(huxerui::Frame{.width = 24.0F, .height = islands.control_height},
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Focusable(false)),
    }.With(huxerui::Frame{.width = 136.0F, .height = islands.control_height},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                               .label = "搜索并选择环境"},
           huxerui::Focusable(true))
        .OnClick([tasks, envSearching, envQuery, currentName = envNames.at(currentEnv)] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                // 搜索框的默认值必须是点击前正在使用的环境，而不是空查询。
                envQuery = huxerui::TextEditingValue{currentName};
                envSearching = true;
            });
        });

    huxerui::View envTrigger = envSearching.Get() ? std::move(envSearch)
                                                  : std::move(envCompact);

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
