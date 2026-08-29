// ui.h — HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app codegen）。
#pragma once

#include <huxerui/huxerui.h>

#include <string>

namespace apitab::ui {

// common.cpp
huxerui::View PageHeader(std::string title, std::string subtitle);
huxerui::View MigrationPlaceholder(std::string pageName);

// home_page.cpp
huxerui::View HomePage(huxerui::State<std::size_t> selected, huxerui::State<std::int64_t> opened);

// request_page.cpp
huxerui::View RequestPage(huxerui::State<std::int64_t> opened);

// loadtest_page.cpp
huxerui::View LoadTestPage();

// websocket_page.cpp
huxerui::View WebSocketPage();

// tcp_page.cpp
huxerui::View TcpPage();

// history_page.cpp
huxerui::View HistoryPage();

// settings_page.cpp（dark 主题开关由 AppRoot 持有）
huxerui::View SettingsPage(huxerui::State<bool> dark);

} // namespace apitab::ui
