// history_page.cpp — 历史记录：分页列表 + 清空（Dialog 确认）。
// entries 每次组合按 page 快照读取；翻页/清空通过 State 驱动重组。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.db;
import apitab.store.requests;

namespace apitab::ui {

namespace {
constexpr int kPageSize = 20;
} // namespace

[[huxerui::composable]] huxerui::View HistoryPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    auto pageIndex = huxerui::UseState<std::int64_t>(0);
    auto reloadKey = huxerui::UseState(0);

    huxerui::Lifecycle(
        [=] -> void {}, reloadKey.Get(), pageIndex.Get());

    const std::int64_t page = pageIndex.Get();
    const std::vector<db::HistoryEntry> entries = g_requests.historyPage(kPageSize, static_cast<int>(page));
    const std::int64_t total = g_requests.historyCount();

    std::vector<huxerui::View> children{
        PageHeader("历史记录", "共 " + std::to_string(total) + " 条请求"),
        huxerui::Button("清空历史").OnClick([dialog, reloadKey, pageIndex] {
            dialog.Show("清空历史", "确定删除全部历史记录吗？此操作不可恢复。", "清空", "取消",
                        [reloadKey, pageIndex] {
                            g_requests.clearHistory();
                            pageIndex = 0;
                            reloadKey = reloadKey.Get() + 1;
                        });
        }),
    };

    for (const db::HistoryEntry& e : entries) {
        const std::string status = e.error.empty() ? std::to_string(e.status) : "ERR";
        children.push_back(
            huxerui::Text(std::format("{}  {}  ·  {}  ·  {}", e.method, e.url, status, e.durationMs),
                          huxerui::TextRole::Body)
                .With(huxerui::Padding(theme.spacing.medium),
                      huxerui::Background(theme.colors.surface_container_low),
                      huxerui::CornerRadius(theme.shapes.small)));
    }

    children.push_back(
        huxerui::Row {
            huxerui::Button("上一页").OnClick([page, pageIndex] {
                if (page > 0) pageIndex = page - 1;
            }),
            huxerui::Button("下一页").OnClick([page, pageIndex] { pageIndex = page + 1; }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)));

    return huxerui::ScrollView{huxerui::Column(std::move(children))
                                   .With(huxerui::Padding(theme.spacing.large),
                                         huxerui::Spacing(theme.spacing.medium))}.With(huxerui::ScrollBar());
}

} // namespace apitab::ui
