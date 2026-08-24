// ui/history_page.cppm — 历史页：单次请求历史记录表 + 清空。
module;

#include "eui_ui.h"

export module apitab.ui.history_page;

import std;
import apitab.db;
import apitab.store.requests;  // g_requests
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

// 历史单页缓存（新响应落库 / 清空 / 翻页后置 dirty；不每帧查库）。
std::vector<db::HistoryEntry> g_history;
bool g_historyDirty = true;
std::int64_t g_historyTotal = 0;
std::int64_t g_historyPage = 0;  // 0-based
int g_historyPageSize = 10;
std::string g_historyPageText = "1";
bool g_historySizeOpen = false;

// app.cpp 在新响应落库后调用。
export void markHistoryDirty() { g_historyDirty = true; }

namespace {

int pageCount() {
    return std::max(1, static_cast<int>((g_historyTotal + g_historyPageSize - 1) / g_historyPageSize));
}

void reloadHistory() {
    g_historyTotal = g_requests.historyCount();
    const std::int64_t maxPage = std::max<std::int64_t>(0, (g_historyTotal - 1) / g_historyPageSize);
    g_historyPage = std::clamp(g_historyPage, std::int64_t{0}, maxPage);
    g_history = g_requests.historyPage(g_historyPageSize, g_historyPage);
    g_historyPageText = std::to_string(g_historyPage + 1);
    g_historyDirty = false;
}

void jumpToPage() {
    try {
        std::size_t parsed = 0;
        const std::string text = trim(g_historyPageText);
        const long long page = std::stoll(text, &parsed);
        if (parsed != text.size()) throw std::invalid_argument("page");
        g_historyPage = std::clamp<std::int64_t>(page - 1, 0, pageCount() - 1);
    } catch (...) {
        g_historyPage = std::clamp<std::int64_t>(g_historyPage, 0, pageCount() - 1);
    }
    g_historyDirty = true;
}

} // namespace

export void drawHistoryPage(eui::Ui& ui, float x, float y, float w, float h,
                            const AppTheme& theme) {
    const auto& tokens = theme.components;
    if (g_historyDirty) reloadHistory();

    ui.text("history.title")
        .position(x, y).size(w - 80.0f, 22.0f)
        .text(std::format("请求历史（{} 条）", g_historyTotal))
        .fontSize(kFontBody + 1.0f).lineHeight(22.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    components::button(ui, "history.clear")
        .position(x + w - 70.0f, y).size(70.0f, 22.0f)
        .icon(0xF1F8).text("清空").fontSize(kFontLabel).theme(tokens, false)
        .onClick([] {
            g_requests.clearHistory();
            g_historyPage = 0;
            g_historyPageText = "1";
            g_historyDirty = true;
            showStatus("历史已清空");
        }).build();

    std::vector<std::vector<std::string>> rows;
    for (const auto& e : g_history) {
        rows.push_back({formatTime(e.createdAt), e.method, e.url,
                        e.error.empty() ? std::to_string(e.status) : ("ERR " + e.error),
                        formatMs(e.durationMs), formatBytes(e.sizeBytes)});
    }
    constexpr float controlsH = 30.0f;
    const float tableY = y + 28.0f;
    const float tableH = std::max(30.0f, h - 28.0f - controlsH);
    ui.stack("history.table.wrap")
        .position(x, tableY).size(w, tableH)
        .content([&] {
            components::dataTable(ui, "history.table")
                .size(w, tableH)
                .columns({"时间", "方法", "URL", "状态", "耗时", "大小"})
                .rows(std::move(rows)).theme(tokens).build();
        }).build();

    const float controlsY = tableY + tableH + 4.0f;
    ui.stack("history.size.wrap")
        .position(x, controlsY).size(86.0f, 24.0f).zIndex(20)
        .content([&] {
            components::dropdown(ui, "history.size")
                .size(86.0f, 24.0f).items({"10", "20", "50", "100"})
                .selected(g_historyPageSize == 10 ? 0 : g_historyPageSize == 20 ? 1 :
                          g_historyPageSize == 50 ? 2 : 3)
                .open(g_historySizeOpen).theme(tokens)
                .onOpenChange([](bool open) { g_historySizeOpen = open; })
                .onChange([](int index) {
                    constexpr int sizes[] = {10, 20, 50, 100};
                    g_historyPageSize = sizes[std::clamp(index, 0, 3)];
                    g_historyPage = 0;
                    g_historySizeOpen = false;
                    g_historyDirty = true;
                }).build();
        }).build();
    ui.text("history.size.label")
        .position(x + 92.0f, controlsY).size(42.0f, 24.0f).text("/ 页")
        .fontSize(kFontLabel).lineHeight(24.0f).color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center).build();
    const int pages = pageCount();
    const bool first = g_historyPage == 0;
    const bool last = g_historyPage >= pages - 1;
    components::button(ui, "history.prev")
        .position(x + w - 192.0f, controlsY).size(24.0f, 24.0f)
        .icon(0xF053).text("").iconSize(8.0f).theme(tokens, false).disabled(first)
        .onClick([] { if (g_historyPage > 0) { --g_historyPage; g_historyDirty = true; } }).build();
    components::input(ui, "history.page.input")
        .position(x + w - 162.0f, controlsY).size(42.0f, 24.0f)
        .value(g_historyPageText).theme(tokens)
        .onChange([](const std::string& value) { g_historyPageText = value; })
        .onEnter([] { jumpToPage(); }).build();
    ui.text("history.page.total")
        .position(x + w - 114.0f, controlsY).size(44.0f, 24.0f)
        .text("/ " + std::to_string(pages)).fontSize(kFontLabel).lineHeight(24.0f)
        .color(theme.metaText).verticalAlign(core::VerticalAlign::Center).build();
    components::button(ui, "history.next")
        .position(x + w - 30.0f, controlsY).size(24.0f, 24.0f)
        .icon(0xF054).text("").iconSize(8.0f).theme(tokens, false).disabled(last)
        .onClick([] { if (g_historyPage + 1 < pageCount()) { ++g_historyPage; g_historyDirty = true; } }).build();
}
