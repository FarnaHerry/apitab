// app.cpp — HuxerUI 前端根：侧栏导航 + 页面切换 + 主题 + 项目工作区上下文。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"

import apitab.preferences;
import apitab.store.requests;
import apitab.store.loadtest;

namespace apitab::ui {

namespace pages {

enum PageIndex : std::size_t {
    kHome = 0,
    kRequest = 1,
    kLoad = 2,
    kWebSocket = 3,
    kTcp = 4,
    kHistory = 5,
    kSettings = 6,
};

[[huxerui::composable]] huxerui::View PageFor(std::size_t index,
                                              huxerui::State<std::size_t> selected,
                                              huxerui::State<std::int64_t> opened,
                                              huxerui::State<bool> dark) {
    switch (index) {
        case kHome:
            return HomePage(selected, opened);
        case kRequest:
            return opened.Get() != 0
                       ? RequestPage(opened)
                       : MigrationPlaceholder("请求（先在主页打开项目，集合操作依赖项目）");
        case kLoad:
            return LoadTestPage();
        case kWebSocket:
            return WebSocketPage();
        case kTcp:
            return TcpPage();
        case kHistory:
            return HistoryPage();
        default:
            return SettingsPage(dark);
    }
}
} // namespace pages

// 应用根：全部页面级状态（官方 README 形态：根标注 composable）。
[[huxerui::composable]] huxerui::View AppRoot() {
    auto selected = huxerui::UseState<std::size_t>(pages::kHome);
    auto dark = huxerui::UseState(sessionPreference("dark") == "1");
    // 显式打开的项目（0 = 未打开）。不做静默恢复：依赖项目的操作必须先打开。
    auto opened = huxerui::UseState<std::int64_t>(0);

    huxerui::View content = huxerui::Row{
        huxerui::NavigationPane(
            {
                huxerui::NavigationItem("主页"),
                huxerui::NavigationItem("请求"),
                huxerui::NavigationItem("压测"),
                huxerui::NavigationItem("WebSocket"),
                huxerui::NavigationItem("TCP"),
                huxerui::NavigationItem("历史"),
                huxerui::NavigationItem("设置"),
            },
            selected, true)
            .OnChanged([selected](std::size_t index) { selected = index; }),
        pages::PageFor(selected.Get(), selected, opened, dark).Key(selected.Get())};

    // 主题跟随设置页开关（会话持久化）。
    return dark.Get() ? huxerui::View{huxerui::MaterialDarkTheme{content}}
                      : huxerui::View{huxerui::MaterialTheme{content}};
}

} // namespace apitab::ui
