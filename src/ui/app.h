// app.h — apitab 应用根声明（定义在 app.cpp，HuxerUI composable 函数）。
#pragma once

#include <huxerui/huxerui.h>

namespace apitab::ui {

// 应用根：侧栏导航 + 页面切换。由 app_main.cpp 注册到 Application。
huxerui::View AppRoot();

} // namespace apitab::ui
