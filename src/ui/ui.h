// ui.h — HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app codegen）。
#pragma once

#include <huxerui/huxerui.h>

#include <cstddef>
#include <string>

namespace apitab::ui {

// common.cpp
huxerui::View PageHeader(std::string title, std::string subtitle);
huxerui::View MigrationPlaceholder(std::string pageName);
huxerui::View EmptyState(huxerui::ImageResource icon, std::string title, std::string subtitle,
                         huxerui::State<std::size_t> navPage);

// home_page.cpp
huxerui::View HomePage(huxerui::State<std::size_t> navPage,
                       huxerui::State<std::vector<std::int64_t>> tabs,
                       huxerui::State<std::int64_t> activeProject);

// request_page.cpp
huxerui::View RequestPage(huxerui::State<std::int64_t> activeProject);

// loadtest_page.cpp
huxerui::View LoadTestPage();

// websocket_page.cpp
huxerui::View WebSocketPage();

// tcp_page.cpp
huxerui::View TcpPage();

// history_page.cpp
huxerui::View HistoryPage();

// settings_page.cpp（全局设置：主题模式 + 关闭行为，状态由 AppRoot 持有）
huxerui::View GlobalSettingsPage(huxerui::State<int> themeMode, huxerui::State<int> closeBehavior);

// project_settings_page.cpp（当前项目设置）
huxerui::View ProjectSettingsPage();

} // namespace apitab::ui
