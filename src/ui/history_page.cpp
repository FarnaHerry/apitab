// history_page.cpp — 历史记录：分页列表 + 清空（Dialog 确认）。
// entries 每次组合按 page 快照读取；翻页/清空通过 State 驱动重组。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.db;
import apitab.store.requests;

namespace apitab::ui {

namespace {
// 每页条数选项（下标即 pageSize 状态值）；默认 10 条。
constexpr std::array<int, 4> kPageSizeOptions{10, 20, 50, 100};
constexpr std::size_t kDefaultPageSizeIndex = 0;

// 历史列表行（P1-A2 布局契约：[前置区] [主内容 Grow] [尾部信息] [固定动作区]）：
// 前置区 = HTTP 方法，主内容 = URL（左对齐、Grow 撑开），尾部信息 = 状态码与
// 耗时（整体贴行右缘、次级色调 on_surface_variant）。
// 固定动作区：**本列表统一不保留动作槽占位**（TrailingActionGroup 占位与否由
// 列表整体决定，禁止同一列表混用）——历史行是纯只读条目，无行级/行尾动作，
// 故整列表决定不放动作区；各行尾部信息右对齐规则一致（URL 区 Grow 撑开把
// 尾部信息推到行右缘），行间不跳动。URL 过长时在主内容区内自然换行/截断
// （Grow 给出有界宽度 + ClipChildren 兜底），不会把尾部信息挤出可视区。
// 行底色/圆角维持现状（surface_container + small 圆角），本工作包只动布局、
// 不动配色体系；行内无焦点件，列表焦点序不受影响。
[[huxerui::composable]] huxerui::View HistoryRow(const db::HistoryEntry& entry) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string status = entry.error.empty() ? std::to_string(entry.status) : "ERR";
    return huxerui::Row {
        // 前置区：HTTP 方法。
        huxerui::Text(entry.method, huxerui::TextRole::Body),
        // 主内容：URL，Grow 撑开（有界宽度内自然换行，ClipChildren 兜底截断）。
        huxerui::Text(entry.url, huxerui::TextRole::Body)
            .With(huxerui::Grow(1.0F), huxerui::ClipChildren()),
        // 尾部信息：状态码 · 耗时，整体右对齐、次级色调。
        huxerui::Text(std::format("{} · {}", status, entry.durationMs), huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Padding(theme.spacing.medium),
              // 条目在岛（container_low）之上，用高一层级的容器底保持可见层次。
              huxerui::Background(theme.colors.surface_container),
              huxerui::CornerRadius(theme.shapes.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}
} // namespace

[[huxerui::composable]] huxerui::View HistoryPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    auto pageIndex = huxerui::UseState<std::int64_t>(0);
    auto pageSize = huxerui::UseState(kDefaultPageSizeIndex);
    auto reloadKey = huxerui::UseState(0);
    // 页码输入框受控值（1 起始显示）；与 pageIndex 在各 handler 中同步更新。
    auto pageInput = huxerui::UseState(huxerui::TextEditingValue::FromText("1"));

    huxerui::Lifecycle(
        [=] -> void {}, reloadKey.Get(), pageIndex.Get(), pageSize.Get());

    const std::int64_t page = pageIndex.Get();
    const int pageSizeValue = kPageSizeOptions.at(pageSize.Get());
    const std::vector<db::HistoryEntry> entries =
        g_requests.historyPage(pageSizeValue, static_cast<int>(page));
    const std::int64_t total = g_requests.historyCount();

    std::vector<huxerui::View> rows;
    for (const db::HistoryEntry& e : entries) {
        rows.push_back(HistoryRow(e).Key(e.id)); // 分页换页的动态兄弟用稳定 Key
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
            huxerui::Button("清空历史").OnClick([dialog, reloadKey, pageIndex, pageInput] {
                ShowDangerConfirm(dialog, "清空历史", "确定删除全部历史记录吗？此操作不可恢复。",
                                  "清空", [reloadKey, pageIndex, pageInput] {
                                      g_requests.clearHistory();
                                      pageIndex = 0;
                                      pageInput = huxerui::TextEditingValue::FromText("1");
                                      reloadKey = reloadKey.Get() + 1;
                                  });
            }),
        },
        huxerui::ScrollView{huxerui::Column(std::move(rows))
                                .With(huxerui::Spacing(theme.spacing.small),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
        // 底部分页栏：< 页码输入(回车跳转) > + 每页条数官方 Select（弹出方向自管理）。
        huxerui::Row {
            huxerui::Button("<").OnClick([page, pageIndex, pageInput] {
                if (page > 0) {
                    pageIndex = page - 1;
                    pageInput = huxerui::TextEditingValue::FromText(std::to_string(page));
                }
            }),
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
            huxerui::Button(">").OnClick([page, pageIndex, pageInput] {
                pageIndex = page + 1;
                pageInput = huxerui::TextEditingValue::FromText(std::to_string(page + 2));
            }),
            huxerui::Text("每页", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            huxerui::Select(std::vector<std::string>{"10", "20", "50", "100"}, pageSize.Get(),
                            [](const std::string& option) {
                                return huxerui::Text(option).Key(option);
                            })
                .OnChanged([pageSize, pageIndex, pageInput](std::size_t index) {
                    pageSize = index;
                    pageIndex = 0; // 条数变化后回到第一页
                    pageInput = huxerui::TextEditingValue::FromText("1");
                }),
        }
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
