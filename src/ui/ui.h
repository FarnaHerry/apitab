// ui.h — HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app codegen）。
#pragma once

#include <huxerui/huxerui.h>

#include <string>

namespace apitab::ui {

// common.cpp
huxerui::View PageHeader(std::string title, std::string subtitle);
huxerui::View MigrationPlaceholder(std::string pageName);

// home_page.cpp
huxerui::View HomePage();

// request_page.cpp
huxerui::View RequestPage();

} // namespace apitab::ui
