// history_page.cpp — 历史记录：左侧按请求分组导航，右侧多标签只读详情。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import apitab.db;
import apitab.store.loadtest;
import apitab.store.requests;
import apitab.utils;

namespace apitab::ui {

namespace {

constexpr std::array<int, 4> kPageSizeOptions{10, 20, 50, 100};
constexpr std::size_t kDefaultPageSizeIndex = 0;
constexpr float kPageSizeSelectWidth = 80.0F;
constexpr float kNavigationWidth = 320.0F;
constexpr float kCompactNavigationHeight = 300.0F;
constexpr int kLoadRecordLimit = 50;

struct HistoryTab {
    std::int64_t id = 0;
    std::string badge;
    std::string title;
    std::variant<db::HistoryEntry, db::LoadRecord> record;

    bool operator==(const HistoryTab& other) const {
        return id == other.id && record.index() == other.record.index();
    }
};

struct HistoryGroup {
    std::string key;
    std::string title;
    std::vector<HistoryTab> entries;
};

std::string RequestHistoryTitle(const db::HistoryEntry& entry) {
    if (const db::SavedRequest* saved = g_requests.find(entry.requestId);
        saved != nullptr && !saved->name.empty()) {
        return saved->name;
    }
    return entry.url.empty() ? "未保存请求" : entry.url;
}

std::string RequestGroupKey(const db::HistoryEntry& entry) {
    if (entry.requestId > 0) return "request:" + std::to_string(entry.requestId);
    return "url:" + entry.method + " " + entry.url;
}

std::string LoadGroupKey(const db::LoadRecord& record) {
    if (record.requestId > 0) return "request:" + std::to_string(record.requestId);
    return "snapshot:" + record.name + " " + record.url;
}

HistoryTab MakeHistoryTab(const db::HistoryEntry& entry) {
    return HistoryTab{
        .id = entry.id,
        .badge = entry.method,
        .title = RequestHistoryTitle(entry),
        .record = entry,
    };
}

HistoryTab MakeHistoryTab(const db::LoadRecord& record) {
    return HistoryTab{
        .id = record.id,
        .badge = "K6",
        .title = record.name.empty() ? record.url : record.name,
        .record = record,
    };
}

template <typename Record>
std::vector<HistoryGroup> BuildHistoryGroups(const std::vector<Record>& records) {
    std::vector<HistoryGroup> groups;
    for (const Record& record : records) {
        const std::string key = [&] {
            if constexpr (std::same_as<Record, db::HistoryEntry>) {
                return RequestGroupKey(record);
            } else {
                return LoadGroupKey(record);
            }
        }();
        auto group = std::ranges::find(groups, key, &HistoryGroup::key);
        if (group == groups.end()) {
            HistoryTab tab = MakeHistoryTab(record);
            groups.push_back(HistoryGroup{.key = key, .title = tab.title});
            group = std::prev(groups.end());
        }
        group->entries.push_back(MakeHistoryTab(record));
    }
    return groups;
}

void OpenHistoryTab(huxerui::State<std::vector<HistoryTab>> tabs,
                    huxerui::State<std::size_t> activeTab, const HistoryTab& tab) {
    std::vector<HistoryTab> copy = tabs.Get();
    for (std::size_t i = 0; i < copy.size(); ++i) {
        if (copy[i].id == tab.id && copy[i].record.index() == tab.record.index()) {
            activeTab = i;
            return;
        }
    }
    copy.push_back(tab);
    const std::size_t newIndex = copy.size() - 1;
    tabs = std::move(copy);
    activeTab = newIndex;
}

void CloseHistoryTab(huxerui::State<std::vector<HistoryTab>> tabs,
                     huxerui::State<std::size_t> activeTab, std::int64_t id) {
    std::vector<HistoryTab> copy = tabs.Get();
    const auto removed = std::ranges::find(copy, id, &HistoryTab::id);
    if (removed == copy.end()) return;
    const std::size_t index = static_cast<std::size_t>(std::distance(copy.begin(), removed));
    const std::size_t active = activeTab.Get();
    copy.erase(removed);
    const std::size_t remaining = copy.size();
    tabs = std::move(copy);
    if (remaining == 0)
        activeTab = 0;
    else if (index == active)
        activeTab = std::min(index, remaining - 1);
    else if (index < active)
        activeTab = active - 1;
}

std::string HistoryEntrySummary(const db::HistoryEntry& entry) {
    const std::string status = entry.error.empty() ? std::to_string(entry.status) : "ERR";
    return "#" + std::to_string(entry.id) + " · " + status + " · " + formatMs(entry.durationMs);
}

std::string LoadRecordSummary(const db::LoadRecord& record) {
    return "#" + std::to_string(record.id) + " · " + std::to_string(record.requests) +
           " req · RPS " + std::format("{:.0f}", record.rps);
}

[[huxerui::composable]] huxerui::View HistoryNavRow(
    HistoryTab tab, huxerui::State<std::vector<HistoryTab>> tabs,
    huxerui::State<std::size_t> activeTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto hovered = huxerui::UseState(false);
    const std::vector<HistoryTab>& openTabs = tabs.Get();
    const bool active = activeTab.Get() < openTabs.size() &&
                        openTabs[activeTab.Get()].id == tab.id &&
                        openTabs[activeTab.Get()].record.index() == tab.record.index();
    const std::string summary = std::visit([](const auto& record) {
        if constexpr (std::same_as<std::decay_t<decltype(record)>, db::HistoryEntry>) {
            return HistoryEntrySummary(record);
        } else {
            return LoadRecordSummary(record);
        }
    }, tab.record);
    return huxerui::Row {
        huxerui::Text(tab.badge, huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{
                .font = huxerui::Font::Monospace(font_size::kCaption)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                .foreground = MethodColor(theme)})
            .With(huxerui::Frame{.min_width = 52.0F}),
        huxerui::Text(summary, huxerui::TextRole::Label)
            .With(huxerui::Grow(1.0F),
                  huxerui::Foreground(active ? theme.colors.on_surface
                                             : theme.colors.on_surface_variant),
                  huxerui::ClipChildren()),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 7.0F)),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              active ? huxerui::Background(islands.active)
                     : hovered.Get() ? huxerui::Background(islands.raised)
                                     : huxerui::Background(huxerui::Color::Transparent()),
              huxerui::Indication{},
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = tab.title + " " + summary})
        .OnClick([tabs, activeTab, tab = std::move(tab)] { OpenHistoryTab(tabs, activeTab, tab); })
        .On<huxerui::ViewEvents::Hover>(
            [hovered](const huxerui::HoverEvent& event) {
                hovered = event.type == huxerui::HoverEventType::Enter;
            });
}

[[huxerui::composable]] huxerui::View HistoryNavigation(
    std::vector<HistoryGroup> groups, huxerui::State<std::vector<HistoryTab>> tabs,
    huxerui::State<std::size_t> activeTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> rows;
    for (const HistoryGroup& group : groups) {
        rows.push_back(
            huxerui::Text(group.title + " · " + std::to_string(group.entries.size()),
                          huxerui::TextRole::Label)
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 6.0F)),
                      huxerui::Foreground(theme.colors.on_surface),
                      huxerui::ClipChildren())
                .Key(group.key));
        for (const HistoryTab& entry : group.entries) {
            rows.push_back(HistoryNavRow(entry, tabs, activeTab).Key(entry.id));
        }
    }
    if (rows.empty()) {
        rows.push_back(huxerui::Text("暂无历史记录", huxerui::TextRole::Body)
                           .With(huxerui::Padding(8.0F),
                                 huxerui::Foreground(theme.colors.on_surface_variant)));
    }
    return huxerui::ScrollView {
        huxerui::Column(std::move(rows))
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::ScrollBar(), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View HistoryChip(
    HistoryTab tab, std::size_t index, huxerui::State<std::vector<HistoryTab>> tabs,
    huxerui::State<std::size_t> activeTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto hovered = huxerui::UseState(false);
    const bool active = index == activeTab.Get();
    const huxerui::Color foreground = active ? theme.colors.on_surface
                                             : theme.colors.on_surface_variant;
    return huxerui::Row {
        huxerui::Text(tab.badge, huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{
                .font = huxerui::Font::Monospace(font_size::kCaption)
                            .WithWeight(huxerui::FontWeight::SemiBold),
                .foreground = MethodColor(theme)}),
        huxerui::Text(tab.title, huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{.font = huxerui::Font::System(font_size::kChip),
                                      .foreground = foreground})
            .With(huxerui::Frame{.max_width = 84.0F}, huxerui::ClipChildren()),
        huxerui::Spacer{},
        AppIconButton(app::images::close, "关闭历史标签",
                      [tabs, activeTab, id = tab.id] { CloseHistoryTab(tabs, activeTab, id); },
                      AppIconButtonShape::Bare, 28.0F, false, hovered.Get())
            .With(huxerui::Opacity(hovered.Get() ? 1.0F : 0.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.extra_small),
              huxerui::Background(active ? theme.colors.surface_container_highest
                                         : hovered.Get() ? theme.colors.surface_container
                                                         : huxerui::Color::Transparent()),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
              huxerui::Frame{.width = 160.0F, .height = 28.0F},
              huxerui::ClipChildren(),
              huxerui::Indication{})
        .OnClick([activeTab, index] { activeTab = index; })
        .On<huxerui::ViewEvents::Hover>(
            [hovered](const huxerui::HoverEvent& event) {
                hovered = event.type != huxerui::HoverEventType::Leave;
            });
}

[[huxerui::composable]] huxerui::View HistoryTabStrip(
    huxerui::State<std::vector<HistoryTab>> tabs, huxerui::State<std::size_t> activeTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::vector<HistoryTab>& snapshot = tabs.Get();
    std::vector<huxerui::View> chips;
    chips.reserve(snapshot.size());
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        chips.push_back(HistoryChip(snapshot[i], i, tabs, activeTab).Key(snapshot[i].id));
    }
    return huxerui::ScrollView {
        huxerui::Row(std::move(chips))
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }
        .ScrollAxis(huxerui::Axis::Horizontal)
        .With(huxerui::ClipChildren(), huxerui::Frame{.height = 32.0F});
}

huxerui::View HistoryDetailField(std::string label, std::string value) {
    return huxerui::Row {
        huxerui::Text(std::move(label), huxerui::TextRole::Label)
            .With(huxerui::Frame{.width = 88.0F}),
        huxerui::Text(std::move(value), huxerui::TextRole::Body)
            .With(huxerui::Grow(1.0F), huxerui::ClipChildren()),
    }
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Start));
}

[[huxerui::composable]] huxerui::View HistoryDetail(HistoryTab tab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> rows;
    rows.push_back(PageHeader(tab.title, "历史记录 #" + std::to_string(tab.id)));
    if (const auto* entry = std::get_if<db::HistoryEntry>(&tab.record)) {
        rows.push_back(HistoryDetailField("请求", entry->method + " " + entry->url));
        rows.push_back(HistoryDetailField(
            "状态", entry->error.empty() ? std::to_string(entry->status) : "传输失败"));
        rows.push_back(HistoryDetailField("耗时", formatMs(entry->durationMs)));
        rows.push_back(HistoryDetailField("大小", formatBytes(entry->sizeBytes)));
        if (!entry->error.empty()) {
            rows.push_back(HistoryDetailField("错误", entry->error));
        }
    } else {
        const db::LoadRecord& record = std::get<db::LoadRecord>(tab.record);
        const std::string mode = record.mode == "stages" ? "阶梯压测"
                                 : record.mode == "iterations" ? "固定迭代"
                                                               : "固定 VUs";
        rows.push_back(HistoryDetailField("URL", record.url));
        rows.push_back(HistoryDetailField("模式", mode));
        rows.push_back(HistoryDetailField(
            "规模", std::format("VU {} · {} · {} 请求", record.vus, record.duration, record.requests)));
        rows.push_back(HistoryDetailField("吞吐", std::format("RPS {:.0f}", record.rps)));
        rows.push_back(HistoryDetailField(
            "延迟", std::format("P50 {:.0f}ms · P90 {:.0f}ms · P95 {:.0f}ms · P99 {:.0f}ms",
                                record.p50Ms, record.p90Ms, record.p95Ms, record.p99Ms)));
        rows.push_back(HistoryDetailField("失败率", std::format("{:.2f}%", record.failRate * 100.0)));
        rows.push_back(HistoryDetailField(
            "Checks", std::format("{} 通过 / {} 失败", record.checksPassed, record.checksFailed)));
        if (record.thresholdOk == 0 || record.thresholdOk == 1) {
            rows.push_back(huxerui::Text(record.thresholdOk == 1 ? "✓ 阈值达标" : "✗ 阈值未达标",
                                         huxerui::TextRole::Body)
                               .With(huxerui::Foreground(record.thresholdOk == 1
                                                             ? theme.colors.on_surface
                                                             : theme.colors.error)));
        }
    }
    return huxerui::ScrollView {
        huxerui::Column(std::move(rows))
            .With(huxerui::Padding(theme.spacing.large),
                  huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::ScrollBar(), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View HistoryWorkspace(
    huxerui::State<std::vector<HistoryTab>> tabs, huxerui::State<std::size_t> activeTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::vector<HistoryTab>& snapshot = tabs.Get();
    if (snapshot.empty()) {
        return huxerui::Column {
            huxerui::Text("从左侧选择一条历史记录", huxerui::TextRole::Title),
            huxerui::Text("记录会在右侧以标签页打开，可同时对照多条历史。",
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }
            .With(huxerui::Padding(theme.spacing.large),
                  huxerui::Spacing(theme.spacing.small),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    const std::size_t active = std::min(activeTab.Get(), snapshot.size() - 1);
    return huxerui::Column {
        HistoryTabStrip(tabs, activeTab),
        huxerui::Divider(),
        HistoryDetail(snapshot[active]).Key(snapshot[active].id),
    }
        .With(huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]] huxerui::View HistoryPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    auto category = huxerui::UseState<std::size_t>(0);
    auto pageIndex = huxerui::UseState<std::int64_t>(0);
    auto pageSize = huxerui::UseState(kDefaultPageSizeIndex);
    auto reloadKey = huxerui::UseState(0);
    auto pageInput = huxerui::UseState(huxerui::TextEditingValue::FromText("1"));
    auto requestTabs = huxerui::UseState<std::vector<HistoryTab>>({});
    auto requestActiveTab = huxerui::UseState<std::size_t>(0);
    auto loadTabs = huxerui::UseState<std::vector<HistoryTab>>({});
    auto loadActiveTab = huxerui::UseState<std::size_t>(0);

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
    const std::string subtitle = category.Get() == 0
        ? "共 " + std::to_string(total) + " 条请求"
        : "共 " + std::to_string(loadTotal) + " 条压测记录";
    static_cast<void>(reloadKey.Get());

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
                pageIndex = 0;
                pageInput = huxerui::TextEditingValue::FromText("1");
            })
            .With(huxerui::Frame{.width = kPageSizeSelectWidth}),
    };

    huxerui::View navigationContent;
    if (category.Get() == 0) {
        navigationContent = historyError.empty()
            ? huxerui::View{HistoryNavigation(BuildHistoryGroups(entries), requestTabs,
                                              requestActiveTab)}
            : huxerui::View{huxerui::Text("读取历史记录失败: " + historyError,
                                          huxerui::TextRole::Body)
                                .With(huxerui::Foreground(theme.colors.error),
                                      huxerui::Padding(theme.spacing.small))};
    } else {
        navigationContent = loadsError.empty()
            ? huxerui::View{HistoryNavigation(BuildHistoryGroups(loads), loadTabs, loadActiveTab)}
            : huxerui::View{huxerui::Text("读取压测记录失败: " + loadsError,
                                          huxerui::TextRole::Body)
                                .With(huxerui::Foreground(theme.colors.error),
                                      huxerui::Padding(theme.spacing.small))};
    }

    huxerui::View navigationFooter = huxerui::Row{};
    if (category.Get() == 0) {
        navigationFooter = huxerui::Column {
            huxerui::Row {
                DangerButton(huxerui::Button("清空历史").OnClick(
                    [dialog, reloadKey, pageIndex, pageInput, toast, requestTabs, requestActiveTab] {
                        ShowDangerConfirm(dialog, "清空历史", "确定删除全部历史记录吗？此操作不可恢复。",
                                          "清空", [reloadKey, pageIndex, pageInput, toast,
                                                   requestTabs, requestActiveTab] {
                                              if (auto result = g_requests.clearHistory(); !result) {
                                                  toast.Show("清空历史失败: " + result.error().message);
                                                  return;
                                              }
                                              pageIndex = 0;
                                              pageInput = huxerui::TextEditingValue::FromText("1");
                                              requestTabs = std::vector<HistoryTab>{};
                                              requestActiveTab = 0;
                                              reloadKey = reloadKey.Get() + 1;
                                          });
                    })),
            }
                .With(huxerui::MainAlign(huxerui::MainAxisAlignment::End)),
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
                        std::int64_t target = pageIndex.Get() + 1;
                        try {
                            target = std::clamp<std::int64_t>(std::stoll(pageInput.Get().text), 1, maxPage);
                        } catch (...) {
                        }
                        pageIndex = target - 1;
                        pageInput = huxerui::TextEditingValue::FromText(std::to_string(target));
                    })
                    .With(huxerui::Frame{.width = 64.0F}),
                AppIconButton(app::images::chevron_right, "下一页", [page, pageIndex, pageInput] {
                    pageIndex = page + 1;
                    pageInput = huxerui::TextEditingValue::FromText(std::to_string(page + 2));
                }, AppIconButtonShape::RoundedSquare, 32.0F),
                huxerui::Text("每页", huxerui::TextRole::Body)
                    .With(huxerui::Foreground(theme.colors.on_surface_variant)),
                std::move(pageSizeSelect),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::MainAlign(huxerui::MainAxisAlignment::End),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    huxerui::View navigation = huxerui::Column {
        PageHeader("历史记录", subtitle),
        huxerui::SegmentedButton({"请求历史", "压测历史"}, category.Get())
            .OnChanged([category](std::size_t index) { category = index; }),
        std::move(navigationContent).With(huxerui::Grow(1.0F)),
        std::move(navigationFooter),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    huxerui::View workspace = category.Get() == 0
        ? huxerui::View{HistoryWorkspace(requestTabs, requestActiveTab)}
        : huxerui::View{HistoryWorkspace(loadTabs, loadActiveTab)};

    huxerui::View contentLayout;
    if (compact) {
        contentLayout = huxerui::Column {
            std::move(navigation).With(huxerui::Frame{.height = kCompactNavigationHeight}),
            huxerui::Row{}.With(huxerui::Frame{.height = kLineWidth},
                                huxerui::Background(islands.line_subtle)),
            std::move(workspace).With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    } else {
        contentLayout = huxerui::Row {
            std::move(navigation).With(huxerui::Frame{.width = kNavigationWidth}),
            huxerui::Column{}.With(huxerui::Frame{.width = kLineWidth},
                                   huxerui::Background(islands.line_subtle)),
            std::move(workspace)
                .With(huxerui::Padding(huxerui::EdgeInsets{.left = theme.spacing.medium}),
                      huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(0.0F),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    const float outerPadding = compact ? theme.spacing.small : theme.spacing.large;
    return huxerui::Column {
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
              huxerui::Frame{.min_width = 320.0F, .min_height = 320.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
