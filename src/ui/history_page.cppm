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

export void drawHistoryPage(eui::Ui& ui, const eui::Screen& screen, float x, float y,
                            float w, float h, const AppTheme& theme) {
    const auto& tokens = theme.components;
    if (g_historyDirty) reloadHistory();

    // 岛屿精确覆盖页面 bounds，内容统一内缩 kPanelPad。
    drawIslandPanel(ui, "history.island", x, y, w, h, theme,
                    theme.dark ? 0.68f : 0.86f);
    const float innerX = x + kPanelPad;
    const float innerY = y + kPanelPad;
    const float innerW = nonNegative(w - kPanelPad * 2.0f);
    const float innerH = nonNegative(h - kPanelPad * 2.0f);

    ui.text("history.title")
        .position(innerX, innerY).size(nonNegative(innerW - 80.0f), 22.0f)
        .text(std::format("请求历史（{} 条）", g_historyTotal))
        .fontSize(kFontBody + 1.0f).lineHeight(22.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    if (innerW >= 80.0f) {
        components::button(ui, "history.clear")
            .position(innerX + innerW - 70.0f, innerY).size(70.0f, 22.0f)
            .icon(0xF1F8).text("清空").fontSize(kFontLabel).theme(tokens, false)
            .radius(kButtonRadius)
            .onClick([] {
                g_requests.clearHistory();
                g_historyPage = 0;
                g_historyPageText = "1";
                g_historyDirty = true;
                showStatus("历史已清空");
            }).build();
    }

    // dataTable 是等宽列且单元格/表头文本都不裁剪：长 URL / 错误信息会溢出到
    // 下一列。按列文本可用宽（列宽 - 2*textInset）省略号截断，保持列内收敛。
    const float cellTextW = nonNegative(
        (innerW - tokens.metrics.spacing.hairline * 2.0f) / 6.0f
        - tokens.metrics.spacing.section * 2.0f);
    const float cellFont = tokens.metrics.typography.label;
    auto fit = [&](const std::string& s) { return fitTextToWidth(s, cellTextW, cellFont); };
    std::vector<std::vector<std::string>> rows;
    for (const auto& e : g_history) {
        rows.push_back({fit(formatTime(e.createdAt)),
                        fit(e.method),
                        fit(e.url),
                        fit(e.error.empty() ? std::to_string(e.status)
                                            : ("ERR " + e.error)),
                        fit(formatMs(e.durationMs)), fit(formatBytes(e.sizeBytes))});
    }
    // 控制区固定在岛底：宽窗口一行（左 picker / 右分页），窄窗口两行。
    // 右组 = prev24 + input42 + total44 + next24 + 3*gap = 152；左组 134。
    const bool narrowControls = innerW < 292.0f;
    const float controlsBlockH = narrowControls ? 52.0f : 24.0f;
    const float controlsY = innerY + nonNegative(innerH - controlsBlockH);
    const float tableY = innerY + 28.0f;
    const float tableH = nonNegative(controlsY - tableY - 4.0f);
    if (tableH > 0.0f) {
        components::scrollView(ui, "history.table.scroll")
            .position(innerX, tableY).size(innerW, tableH)
            .theme(tokens)
            .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
            .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
                const float rowW = nonNegative(contentWidth);
                // dataTable 本身是固定高度组件；给它真实的行高总和，scrollView
                // 才能在每页记录超过视口时产生有效滚动范围。
                constexpr float headerH = 28.0f;
                constexpr float rowH = 24.0f;
                const float contentH = std::max(viewportH,
                    headerH + static_cast<float>(rows.size()) * rowH);
                cu.stack("history.table.content").size(rowW, contentH).content([&] {
                    components::dataTable(cu, "history.table")
                        .size(rowW, contentH)
                        .columns({fit("时间"), fit("方法"), fit("URL"),
                                  fit("状态"), fit("耗时"), fit("大小")})
                        .rows(rows).theme(tokens).build();
                }).build();
            })
            .build();
    }

    ui.stack("history.size.wrap")
        .position(innerX, controlsY).size(86.0f, 24.0f).zIndex(20)
        .content([&] {
            const std::vector<std::string> sizes = {"10", "20", "50", "100"};
            const int selected = g_historyPageSize == 10 ? 0 : g_historyPageSize == 20 ? 1 :
                                 g_historyPageSize == 50 ? 2 : 3;
            drawListPicker(ui, "history.size", 86.0f, 24.0f, theme, g_historySizeOpen,
                           sizes, selected, true, [](int index) {
                               constexpr int pageSizes[] = {10, 20, 50, 100};
                               g_historyPageSize = pageSizes[std::clamp(index, 0, 3)];
                               g_historyPage = 0;
                               g_historyDirty = true;
                           }, controlsY, screen.height);
        }).build();
    ui.text("history.size.label")
        .position(innerX + 92.0f, controlsY).size(42.0f, 24.0f).text("/ 页")
        .fontSize(kFontLabel).lineHeight(24.0f).color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center).build();
    const int pages = pageCount();
    const bool first = g_historyPage == 0;
    const bool last = g_historyPage >= pages - 1;
    const float pgY = narrowControls ? controlsY + 28.0f : controlsY;
    const float pgX = innerX + nonNegative(innerW - 152.0f);
    components::button(ui, "history.prev")
        .position(pgX, pgY).size(24.0f, 24.0f)
        .icon(0xF053).text("").iconSize(8.0f).theme(tokens, false).radius(12.0f).disabled(first)
        .onClick([] { if (g_historyPage > 0) { --g_historyPage; g_historyDirty = true; } }).build();
    components::input(ui, "history.page.input")
        .position(pgX + 30.0f, pgY).size(42.0f, 24.0f)
        .value(g_historyPageText).theme(tokens)
        .onChange([](const std::string& value) { g_historyPageText = value; })
        .onEnter([] { jumpToPage(); }).build();
    ui.text("history.page.total")
        .position(pgX + 78.0f, pgY).size(44.0f, 24.0f)
        .text("/ " + std::to_string(pages)).fontSize(kFontLabel).lineHeight(24.0f)
        .color(theme.metaText).verticalAlign(core::VerticalAlign::Center).build();
    components::button(ui, "history.next")
        .position(pgX + 128.0f, pgY).size(24.0f, 24.0f)
        .icon(0xF054).text("").iconSize(8.0f).theme(tokens, false).disabled(last)
        .onClick([] { if (g_historyPage + 1 < pageCount()) { ++g_historyPage; g_historyDirty = true; } }).build();
}
