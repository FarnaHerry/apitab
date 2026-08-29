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

    std::vector<huxerui::View> rows;
    for (const db::HistoryEntry& e : entries) {
        const std::string status = e.error.empty() ? std::to_string(e.status) : "ERR";
        rows.push_back(
            huxerui::Text(std::format("{}  {}  ·  {}  ·  {}", e.method, e.url, status, e.durationMs),
                          huxerui::TextRole::Body)
                .With(huxerui::Padding(theme.spacing.medium),
                      // 条目在岛（container_low）之上，用高一层级的容器底保持可见层次。
                      huxerui::Background(theme.colors.surface_container),
                      huxerui::CornerRadius(theme.shapes.small)));
    }
    if (entries.empty()) {
        rows.push_back(huxerui::Text("暂无历史记录", huxerui::TextRole::Body)
                           .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }

    // 岛屿分区模型：本页只有一个岛，岛本身占满整个页面区块（Grow + Stretch），
    // 列表在岛内部滚动；分页按钮固定在岛底部。
    // 清空按钮套一层 Row：岛交叉轴 Stretch 会把直接子节点拉满全宽，
    // Row 主轴不拉伸子节点，按钮保持自然宽度。
    return huxerui::Column {
        PageHeader("历史记录", "共 " + std::to_string(total) + " 条请求"),
        huxerui::Row {
            huxerui::Button("清空历史").OnClick([dialog, reloadKey, pageIndex] {
                dialog.Show("清空历史", "确定删除全部历史记录吗？此操作不可恢复。", "清空", "取消",
                            [reloadKey, pageIndex] {
                                g_requests.clearHistory();
                                pageIndex = 0;
                                reloadKey = reloadKey.Get() + 1;
                            });
            }),
        },
        huxerui::ScrollView{huxerui::Column(std::move(rows))
                                .With(huxerui::Spacing(theme.spacing.small),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
        huxerui::Row {
            huxerui::Button("上一页").OnClick([page, pageIndex] {
                if (page > 0) pageIndex = page - 1;
            }),
            huxerui::Button("下一页").OnClick([page, pageIndex] { pageIndex = page + 1; }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
