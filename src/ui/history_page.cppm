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

// 历史缓存（新响应落库 / 清空后置 dirty；不每帧查库）。
std::vector<db::HistoryEntry> g_history;
bool g_historyDirty = true;

// app.cpp 在新响应落库后调用。
export void markHistoryDirty() { g_historyDirty = true; }

export void drawHistoryPage(eui::Ui& ui, float x, float y, float w, float h,
                            const AppTheme& theme) {
    const auto& tokens = theme.components;

    if (g_historyDirty) {
        g_history = g_requests.history();
        g_historyDirty = false;
    }

    ui.text("history.title")
        .position(x, y)
        .size(w - 80.0f, 22.0f)
        .text(std::format("请求历史（{} 条）", g_history.size()))
        .fontSize(kFontBody + 1.0f)
        .lineHeight(22.0f)
        .color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
    components::button(ui, "history.clear")
        .position(x + w - 70.0f, y)
        .size(70.0f, 22.0f)
        .icon(0xF1F8)
        .text("清空")
        .fontSize(kFontLabel)
        .theme(tokens, false)
        .onClick([] {
            // clearHistory 走 store（保持 UI 不直接碰 db 句柄的纪律）
            g_requests.clearHistory();
            g_historyDirty = true;
            showStatus("历史已清空");
        })
        .build();

    std::vector<std::vector<std::string>> rows;
    for (const auto& e : g_history) {
        rows.push_back({
            formatTime(e.createdAt),
            e.method,
            e.url,
            e.error.empty() ? std::to_string(e.status) : ("ERR " + e.error),
            formatMs(e.durationMs),
            formatBytes(e.sizeBytes),
        });
    }
    ui.stack("history.table.wrap")
        .position(x, y + 28.0f)
        .size(w, h - 28.0f)
        .content([&] {
            components::dataTable(ui, "history.table")
                .size(w, h - 28.0f)
                .columns({"时间", "方法", "URL", "状态", "耗时", "大小"})
                .rows(std::move(rows))
                .theme(tokens)
                .build();
        })
        .build();
}
