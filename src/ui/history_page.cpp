// history_page.cpp — 历史记录：宽屏左侧分类栏、窄屏顶部分类切换 + 记录列表。
// 请求历史保持分页 + 清空（Dialog 确认）；entries 每次组合按 page 快照读取，
// 翻页/清空/切分类通过 State 驱动重组。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import apitab.db;
import apitab.store.loadtest;
import apitab.store.requests;
import apitab.utils;

namespace apitab::ui {

namespace {
// 每页条数选项（下标即 pageSize 状态值）；默认 10 条。
constexpr std::array<int, 4> kPageSizeOptions{10, 20, 50, 100};
constexpr std::size_t kDefaultPageSizeIndex = 0;
constexpr float kPageSizeSelectWidth = 80.0F;
// 宽屏分类栏宽度（工具侧栏节奏）；压测记录列表一次读最近 50 条。
constexpr float kCategoryRailWidth = 168.0F;
constexpr int kLoadRecordLimit = 50;

// 分类行：与 settings_page 左分类行同一配方（显式压制 OnClick 默认 Indication，
// 整行反馈 = 手工底色：选中 islands.active / hover islands.raised；嵌套档圆角；
// Focusable 参与 Tab 遍历，Enter/Space 激活由框架键盘路径派发）。行尾计数
// 徽标右对齐，选中时提亮为主文本色。
[[huxerui::composable]] huxerui::View HistoryCategoryRow(std::string_view label,
                                                         std::int64_t count, bool selected,
                                                         std::size_t index,
                                                         huxerui::State<std::size_t> category) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto hovered = huxerui::UseState(false);
    return huxerui::Row {
        huxerui::Text(std::string(label), huxerui::TextRole::Label)
            .With(huxerui::Grow(1.0F),
                  huxerui::Foreground(selected ? theme.colors.on_surface
                                               : theme.colors.on_surface_variant)),
        huxerui::Text(std::to_string(count), huxerui::TextRole::Label)
            .With(huxerui::Foreground(selected ? theme.colors.on_surface
                                               : theme.colors.on_surface_variant)),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 9.0F)),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              selected ? huxerui::Background(islands.active)
                       : hovered.Get() ? huxerui::Background(islands.raised)
                                       : huxerui::Background(huxerui::Color::Transparent()),
              huxerui::Indication{}, // 有意压制默认 Indication（见上），整行反馈 = 手工底色
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = std::string(label)})
        .OnClick([category, index] { category = index; })
        .On<huxerui::ViewEvents::Hover>(
            [hovered](const huxerui::HoverEvent& e) {
                hovered = e.type == huxerui::HoverEventType::Enter;
            });
}

// 历史列表行（P1-A2 布局契约：[前置区] [主内容 Grow] [尾部信息] [固定动作区]）：
// 前置区 = HTTP 方法，主内容 = URL（左对齐、Grow 撑开），尾部信息 = 状态码与
// 耗时（整体贴行右缘、次级色调 on_surface_variant）。
// 固定动作区：**本列表统一不保留动作槽占位**（TrailingActionGroup 占位与否由
// 列表整体决定，禁止同一列表混用）——历史行是纯只读条目，无行级/行尾动作，
// 故整列表决定不放动作区；各行尾部信息右对齐规则一致（URL 区 Grow 撑开把
// 尾部信息推到行右缘），行间不跳动。URL 过长时在主内容区内自然换行/截断
// （Grow 给出有界宽度 + ClipChildren 兜底），不会把尾部信息挤出可视区。
// 行样式：与通用表格一致——**不做卡片**（无底色、无圆角），行与行之间用 kLineWidth
// 分隔线切分，行内容左右与页头对齐（列表本身已在岛内，再套一层卡片会重复分层）。
[[huxerui::composable]] huxerui::View HistoryRow(const db::HistoryEntry& entry) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string status = entry.error.empty() ? std::to_string(entry.status) : "ERR";
    huxerui::View content;
    if (huxerui::UseViewportClass() == huxerui::ViewportClass::Compact) {
        content = huxerui::Column {
            huxerui::Row {
                huxerui::Text(entry.method, huxerui::TextRole::Body),
                huxerui::Text(status + " · " + formatMs(entry.durationMs),
                              huxerui::TextRole::Body)
                    .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            }.With(huxerui::Spacing(theme.spacing.small),
                   huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
            huxerui::Text(entry.url, huxerui::TextRole::Body)
                .With(huxerui::ClipChildren()),
        }.With(huxerui::Spacing(theme.spacing.extra_small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        content = huxerui::Row {
            // 前置区：HTTP 方法。
            huxerui::Text(entry.method, huxerui::TextRole::Body),
            // 主内容：URL，Grow 撑开（有界宽度内自然换行，ClipChildren 兜底截断）。
            huxerui::Text(entry.url, huxerui::TextRole::Body)
                .With(huxerui::Grow(1.0F), huxerui::ClipChildren()),
            // 尾部信息：状态码 · 耗时，整体右对齐、次级色调。
            huxerui::Text(status + " · " + formatMs(entry.durationMs),
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    // 行首一条分隔线：整列看起来就是表格（含列表顶边），行间不再有卡片间隙。
    return huxerui::Column {
        huxerui::Divider(),
        std::move(content).With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
            0.0F, theme.spacing.small))),
    }.With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 分页结果使用官方 StateList 作为 VirtualList 数据源。分页/清空通过 Key 让本作用域
// 按新快照重建，列表内部不再由父作用域手工拼接 View 向量。
[[huxerui::composable]] huxerui::View HistoryRows(std::vector<db::HistoryEntry> initial) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto entries = huxerui::UseStateList(std::move(initial));
    if (entries.Empty()) {
        return huxerui::Column{
            huxerui::Text("暂无历史记录", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }
            .With(huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    // 列表底部再补一条分隔线收口，与分页栏分开（表格语义：上下都有框线）。
    return huxerui::Column {
        huxerui::VirtualList{
            entries, [](const db::HistoryEntry& entry) {
                return HistoryRow(entry).Key(entry.id);
            }}
            .EstimatedItemExtent(compact ? 64.0F : 40.0F)
            .CacheExtent(160.0F)
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Divider(),
    }
        .With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 压测记录行：宽屏按「脚本名 / URL / 指标」排成一行；窄屏纵向展开，避免指标
// 挤压 URL。列表仍用 Divider 切行，不做卡片。
[[huxerui::composable]] huxerui::View LoadRecordRow(const db::LoadRecord& r, bool compact) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::string shape = r.mode == "stages" ? "阶梯" : r.mode == "iterations" ? "迭代" : "固定";
    std::string tail = std::format("{} VU={} · {} req · RPS {:.0f} · P95 {:.0f}ms", shape,
                                   r.vus, r.requests, r.rps, r.p95Ms);
    if (!r.duration.empty()) tail += " · " + r.duration;
    std::string verdict;
    if (r.thresholdOk == 0 || r.thresholdOk == 1) verdict = r.thresholdOk == 1 ? " · ✓" : " · ✗";
    const huxerui::Color tailColor = r.thresholdOk == 0 ? theme.colors.error
                                                        : theme.colors.on_surface_variant;
    huxerui::View content;
    if (compact) {
        content = huxerui::Column {
            huxerui::Text(r.name, huxerui::TextRole::Body),
            huxerui::Text(r.url, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant), huxerui::ClipChildren()),
            huxerui::Text(verdict.empty() ? tail : tail + verdict, huxerui::TextRole::Body)
                .With(huxerui::Foreground(tailColor), huxerui::ClipChildren()),
        }.With(huxerui::Spacing(theme.spacing.extra_small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        content = huxerui::Row {
            huxerui::Text(r.name, huxerui::TextRole::Body).With(huxerui::ClipChildren()),
            huxerui::Text(r.url, huxerui::TextRole::Body)
                .With(huxerui::Grow(1.0F), huxerui::ClipChildren()),
            huxerui::Text(verdict.empty() ? tail : tail + verdict, huxerui::TextRole::Body)
                .With(huxerui::Foreground(tailColor), huxerui::ClipChildren()),
        }.With(huxerui::Spacing(theme.spacing.small),
               huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
    }
    return huxerui::Column {
        huxerui::Divider(),
        std::move(content).With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
            0.0F, theme.spacing.small))),
    }
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 压测记录列表：最近 50 条（最新在前），表格行 + 上下框线，空态文案。
[[huxerui::composable]] huxerui::View LoadRecordRows(std::vector<db::LoadRecord> initial,
                                                      bool compact) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const auto records = huxerui::UseStateList(std::move(initial));
    if (records.Empty()) {
        return huxerui::Column{
            huxerui::Text("暂无压测记录——在压测页发起一次压测后归档在这里",
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }
            .With(huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return huxerui::Column {
        huxerui::VirtualList{
            records, [compact](const db::LoadRecord& r) { return LoadRecordRow(r, compact).Key(r.id); }}
            .EstimatedItemExtent(compact ? 76.0F : 40.0F)
            .CacheExtent(160.0F)
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
        huxerui::Divider(),
    }
        .With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
} // namespace

[[huxerui::composable]] huxerui::View HistoryPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    // 左栏分类：0 = 请求历史，1 = k6 压测记录。
    auto category = huxerui::UseState<std::size_t>(0);
    auto pageIndex = huxerui::UseState<std::int64_t>(0);
    auto pageSize = huxerui::UseState(kDefaultPageSizeIndex);
    auto reloadKey = huxerui::UseState(0);
    // 页码输入框受控值（1 起始显示）；与 pageIndex 在各 handler 中同步更新。
    auto pageInput = huxerui::UseState(huxerui::TextEditingValue::FromText("1"));

    huxerui::Lifecycle(
        [=] -> void {}, reloadKey.Get(), pageIndex.Get(), pageSize.Get());

    // 双分类数据：计数进左栏徽标；压测记录一次读最近 50 条（最新在前）。
    const std::int64_t page = pageIndex.Get();
    const int pageSizeValue = kPageSizeOptions.at(pageSize.Get());
    const auto entriesResult = g_requests.historyPage(pageSizeValue, page);
    const auto totalResult = g_requests.historyCount();
    const std::vector<db::HistoryEntry> entries = entriesResult
                                                       ? *entriesResult
                                                       : std::vector<db::HistoryEntry>{};
    const std::int64_t total = totalResult ? *totalResult : 0;
    const std::string historyError = !entriesResult
                                         ? entriesResult.error().message
                                         : !totalResult ? totalResult.error().message : std::string{};
    const auto loadsResult = g_loadtest.records(kLoadRecordLimit);
    const auto loadCountResult = g_loadtest.recordCount();
    const std::vector<db::LoadRecord> loads = loadsResult ? *loadsResult
                                                          : std::vector<db::LoadRecord>{};
    const std::int64_t loadTotal = loadCountResult ? *loadCountResult : 0;
    const std::string loadsError =
        !loadsResult ? loadsResult.error().message
                     : !loadCountResult ? loadCountResult.error().message : std::string{};
    const std::string subtitle =
        category.Get() == 0 ? (historyError.empty()
                                   ? "共 " + std::to_string(total) + " 条请求"
                                   : "读取历史记录失败: " + historyError)
                            : (loadsError.empty() ? "最近 " + std::to_string(loads.size()) +
                                                        " / 共 " + std::to_string(loadTotal) + " 条"
                                                  : "读取压测记录失败: " + loadsError);

    // Select 的全局最小宽度为 120dp，对仅含 10/20/50/100 的分页控件过宽；
    // 弹出菜单会至少与触发框同宽，因此在局部主题与触发框同时限制为 80dp。
    huxerui::SelectStyle compactPageSizeStyle = huxerui::UseEnvironment<huxerui::SelectStyle>();
    compactPageSizeStyle.minimum_width = kPageSizeSelectWidth;
    huxerui::ThemeDefinition compactPageSizeTheme;
    compactPageSizeTheme.Set(std::move(compactPageSizeStyle));
    huxerui::View pageSizeSelect = huxerui::Theme{
        std::move(compactPageSizeTheme),
        huxerui::Select(std::vector<std::string>{"10", "20", "50", "100"}, pageSize.Get(),
                         [](const std::string& option) { return huxerui::Text(option).Key(option); })
            .OnChanged([pageSize, pageIndex, pageInput](std::size_t index) {
                pageSize = index;
                pageIndex = 0; // 条数变化后回到第一页
                pageInput = huxerui::TextEditingValue::FromText("1");
            })
            .With(huxerui::Frame{.width = kPageSizeSelectWidth}),
    };

    // 右侧面板按分类分派：请求历史 = 清空 + 表格 + 分页栏；压测记录 = 表格。
    huxerui::View requestPanel = huxerui::Column {
        huxerui::Row {
            DangerButton(huxerui::Button("清空历史").OnClick([dialog, reloadKey, pageIndex, pageInput, toast] {
                ShowDangerConfirm(dialog, "清空历史", "确定删除全部历史记录吗？此操作不可恢复。",
                                  "清空", [reloadKey, pageIndex, pageInput, toast] {
                                      if (auto result = g_requests.clearHistory(); !result) {
                                          toast.Show("清空历史失败: " + result.error().message);
                                          return;
                                      }
                                      pageIndex = 0;
                                      pageInput = huxerui::TextEditingValue::FromText("1");
                                      reloadKey = reloadKey.Get() + 1;
                                  });
            })),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, theme.spacing.small)),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::End),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        historyError.empty()
            ? huxerui::View{HistoryRows(entries)
                                .Key(std::format("{}-{}-{}", reloadKey.Get(), page, pageSizeValue))
                                .With(huxerui::Grow(1.0F))}
            : huxerui::View{huxerui::Text("历史记录暂不可用", huxerui::TextRole::Body)
                                .With(huxerui::Grow(1.0F),
                                      huxerui::Foreground(theme.colors.error))},
        // 底部分页栏：上一页/页码输入(回车跳转)/下一页 + 每页条数官方 Select。
        huxerui::Row {
            AppIconButton(app::images::chevron_left, "上一页", [page, pageIndex, pageInput] {
                if (page > 0) {
                    pageIndex = page - 1;
                    pageInput = huxerui::TextEditingValue::FromText(std::to_string(page));
                }
            }, AppIconButtonShape::RoundedSquare, 32.0F, false, page > 0),
            huxerui::TextField(pageInput)
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([pageInput](const huxerui::TextEditingValue& value) { pageInput = value; })
                .OnSubmitted([pageIndex, pageInput, total, pageSizeValue] {
                    const std::int64_t maxPage =
                        std::max<std::int64_t>(1, (total + pageSizeValue - 1) / pageSizeValue);
                    std::int64_t target = pageIndex.Get() + 1; // 解析失败则保持当前页
                    try {
                        target = std::clamp<std::int64_t>(std::stoll(pageInput.Get().text), 1, maxPage);
                    } catch (...) {
                    }
                    pageIndex = target - 1;
                    pageInput = huxerui::TextEditingValue::FromText(std::to_string(target));
                })
                .With(huxerui::Frame{.width = 72.0F}),
            AppIconButton(app::images::chevron_right, "下一页", [page, pageIndex, pageInput] {
                pageIndex = page + 1;
                pageInput = huxerui::TextEditingValue::FromText(std::to_string(page + 2));
            }, AppIconButtonShape::RoundedSquare, 32.0F),
            huxerui::Text("每页", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            std::move(pageSizeSelect),
        }
        .With(huxerui::Spacing(compact ? theme.spacing.small : theme.spacing.medium),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(0.0F, theme.spacing.small)),
              huxerui::MainAlign(huxerui::MainAxisAlignment::End),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }
        .With(huxerui::Grow(1.0F),
              huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    huxerui::View categoryNavigation;
    if (compact) {
        categoryNavigation = huxerui::Row {
            HistoryCategoryRow("请求历史", total, category.Get() == 0, 0, category)
                .Key("request-history")
                .With(huxerui::Grow(1.0F)),
            HistoryCategoryRow("压测记录", loadTotal, category.Get() == 1, 1, category)
                .Key("load-history")
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.extra_small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        categoryNavigation = huxerui::Column {
            HistoryCategoryRow("请求历史", total, category.Get() == 0, 0, category)
                .Key("request-history"),
            HistoryCategoryRow("压测记录", loadTotal, category.Get() == 1, 1, category)
                .Key("load-history"),
        }
            .With(huxerui::Frame{.width = kCategoryRailWidth},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    huxerui::View selectedPanel = category.Get() == 0
        ? std::move(requestPanel)
        : huxerui::View{LoadRecordRows(std::move(loads), compact).With(huxerui::Grow(1.0F))};

    huxerui::View contentLayout;
    if (compact) {
        contentLayout = huxerui::Column {
            categoryNavigation,
            std::move(selectedPanel),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        contentLayout = huxerui::Row {
            categoryNavigation,
            // 线条风分区：分类栏和记录列表之间由一条通栏 subtle 线分隔。
            huxerui::Column{}.With(huxerui::Frame{.width = kLineWidth},
                                   huxerui::Background(islands.line_subtle)),
            std::move(selectedPanel)
                .With(huxerui::Padding(huxerui::EdgeInsets{.left = theme.spacing.medium})),
        }
            .With(huxerui::Spacing(0.0F),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    // 列表在内容区滚动，分页栏固定在请求历史面板底部；窄窗口把分类切换移到上方。
    const float outerPadding = compact ? theme.spacing.small : theme.spacing.large;
    return huxerui::Column {
        PageHeader("历史记录", subtitle),
        std::move(contentLayout),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets{
                  .top = compact ? theme.spacing.small : theme.spacing.medium,
                  .right = outerPadding,
                  .bottom = outerPadding,
                  .left = outerPadding,
              }),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Background(islands.base),
              huxerui::Grow(1.0F),
              huxerui::Frame{.min_width = 320.0F, .min_height = 220.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
