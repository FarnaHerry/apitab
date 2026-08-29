// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘）：
//   标题栏岛：Logo(AT) + 顶级项目标签条（Grow）+ 齿轮(全局设置) + 框架窗口按钮。
//   下方：左侧图标侧边栏岛（Tooltip 悬停提示）｜内容岛。
// 托盘：托盘图标/菜单（显示主窗口/退出）；关闭行为三选（每次询问/直接关闭/
//   最小化到托盘），未配置时第一次关闭弹窗询问并把选择写入配置。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"
#include "app.h"
#include "app_resources.h"

import apitab.config;
import apitab.db;
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
    kProjectSettings = 6,
    kAppSettings = 7,
};

[[huxerui::composable]] huxerui::View PageFor(std::size_t index,
                                              huxerui::State<std::size_t> navPage,
                                              huxerui::State<std::vector<std::int64_t>> tabs,
                                              huxerui::State<std::int64_t> activeProject,
                                              huxerui::State<int> themeMode,
                                              huxerui::State<int> closeBehavior) {
    switch (index) {
        case kHome:
            return HomePage(navPage, tabs, activeProject);
        case kRequest:
            return activeProject.Get() != 0
                       ? RequestPage(activeProject)
                       : MigrationPlaceholder("请求（在项目标签页内使用；先在主页打开项目）");
        case kLoad:
            return LoadTestPage();
        case kWebSocket:
            return WebSocketPage();
        case kTcp:
            return TcpPage();
        case kHistory:
            return HistoryPage();
        case kProjectSettings:
            return ProjectSettingsPage();
        default:
            return GlobalSettingsPage(themeMode, closeBehavior);
    }
}
} // namespace pages

namespace {

// 软件徽标：字母 AT 合成的圆角块。
[[huxerui::composable]] huxerui::View LogoBadge() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text("AT", huxerui::TextRole::Title)
        .With(huxerui::Frame{.width = 44.0F, .height = 36.0F},
              huxerui::Background(theme.colors.primary),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::Foreground(theme.colors.on_primary),
              huxerui::Align{.horizontal = huxerui::HorizontalAlignment::Center,
                             .vertical = huxerui::VerticalAlignment::Center});
}

// 顶级标签条：第一个为主页（固定不可关），其后每个项目一个可关标签。
[[huxerui::composable]] huxerui::View ProjectTabStrip(
    huxerui::State<std::vector<std::int64_t>> tabs, huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();

    std::vector<std::pair<std::int64_t, std::string>> items;
    items.emplace_back(0, "主页");
    for (const db::Project& p : g_requests.allProjects()) {
        for (std::int64_t id : tabs.Get()) {
            if (id == p.id) items.emplace_back(p.id, p.name);
        }
    }

    auto activateProject = [activeProject](std::int64_t id) {
        if (id != 0) {
            g_requests.selectProject(id);
            g_loadtest.setProject(id);
            saveSessionPreference("active_project", std::to_string(id));
        }
        activeProject = id;
    };

    std::vector<huxerui::View> chips;
    for (const auto& [id, name] : items) {
        const bool active = activeProject.Get() == id;
        const std::string label = (active ? "● " : "") + name;
        chips.push_back(
            huxerui::Row {
                huxerui::Button(label).OnClick([activateProject, id] { activateProject(id); }),
                id != 0 ? huxerui::View{huxerui::Button("✕").OnClick(
                              [=] {
                                  // 关闭会移除本按钮所在行：推迟出指针事件路径
                                  tasks.Launch([=]() -> huxerui::Task<void> {
                                      co_await huxerui::Delay(std::chrono::duration<double>{0});
                                      std::vector<std::int64_t> rest = tabs.Get();
                                      std::erase(rest, id);
                                      tabs = rest;
                                      if (activeProject.Get() == id) activateProject(0);
                                  });
                              })}
                        : huxerui::View{huxerui::Text("")},
            }
                .With(huxerui::Spacing(2.0F)));
    }

    return huxerui::Row(std::move(chips))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 左列岛：图标侧边栏（选中态用实心图标变体，悬停显示文字提示）。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
    };
    const std::array<Item, 7> items{
        Item{app::images::home, app::images::home_selected, "主页"},
        Item{app::images::request, app::images::request_selected, "请求"},
        Item{app::images::loadtest, app::images::loadtest_selected, "压测"},
        Item{app::images::websocket, app::images::websocket_selected, "WebSocket"},
        Item{app::images::tcp, app::images::tcp_selected, "TCP"},
        Item{app::images::history, app::images::history_selected, "历史记录"},
        Item{app::images::project_settings, app::images::project_settings_selected, "项目设置"},
    };

    std::vector<huxerui::View> buttons;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = items[i];
        const huxerui::ImageResource& icon = navPage.Get() == i ? item.icon_selected : item.icon;
        buttons.push_back(huxerui::IconButton(icon, item.tooltip)
                              .OnClick([navPage, i] { navPage = i; })
                              .With(huxerui::Tooltip(item.tooltip)));
    }
    buttons.push_back(
        huxerui::IconButton(app::images::gear, "全局设置")
            .OnClick([navPage] { navPage = pages::kAppSettings; })
            .With(huxerui::Tooltip("全局设置")));

    return huxerui::Column(std::move(buttons))
        .With(huxerui::Padding(theme.spacing.medium), huxerui::Spacing(theme.spacing.small),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Frame{.width = 64.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace

// 应用根：状态全部在这里（官方 README 形态：根标注 composable）。
[[huxerui::composable]] huxerui::View AppRoot() {
    const huxerui::ApplicationHandle application = huxerui::UseApplication();
    const huxerui::WindowHandle window = huxerui::UseWindow();
    const huxerui::SystemTrayHandle tray = application.SystemTray();
    const bool trayAvailable = tray.IsAvailable();
    auto dialog = huxerui::UseDialog();

    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    auto themeMode = huxerui::UseState<int>(0); // 0=跟随系统 1=深色 2=浅色
    // 顶级标签：activeProject = 0 为主页标签；其余值为打开的项目 id。
    auto tabs = huxerui::UseState<std::vector<std::int64_t>>({});
    auto activeProject = huxerui::UseState<std::int64_t>(0);
    // 关闭行为：0=每次询问 1=直接关闭 2=最小化到托盘
    auto closeBehavior = huxerui::UseState<int>(0);
    auto closeDialogOpen = huxerui::UseState(false);
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    if (sessionPreference("theme_mode") == "1") themeMode = 1;
    if (sessionPreference("theme_mode") == "2") themeMode = 2;
    if (sessionPreference("close_behavior") == "1") closeBehavior = 1;
    if (sessionPreference("close_behavior") == "2") closeBehavior = 2;
    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());

    // 托盘：图标 + 菜单；点击托盘图标激活主窗口。
    if (trayAvailable) {
        tray.OnActivate([window] { window.Activate(); });
        huxerui::Lifecycle(
            [tray, window, application] {
                // 菜单项用 push_back 构造：GCC 16 对 menu 列表初始化内的
                // MenuItem/MenuSection 隐式转换报 "expected primary-expression"
                std::vector<huxerui::MenuEntry> menuEntries;
                menuEntries.push_back(
                    huxerui::MenuItem("显示主窗口", [window] { window.Activate(); }));
                menuEntries.push_back(huxerui::MenuSection{});
                menuEntries.push_back(
                    huxerui::MenuItem("退出", [application] { application.Quit(); }));
                tray.Show(app::images::tray,
                          huxerui::SystemTrayOptions{
                              .tooltip = "apitab — API 测试与压测",
                              .menu = std::move(menuEntries)});
                return [tray] { tray.Hide(); };
            },
            0);
    }

    // 关闭拦截：按配置直接关闭/进托盘；未配置时第一次询问并把选择写入配置。
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (!trayAvailable) return false; // 无托盘：交给系统直接关闭
            if (closeBehavior.Get() == 1) return false;
            if (closeBehavior.Get() == 2) {
                window.Hide();
                return true;
            }
            if (closeDialogOpen.Get()) return true;
            closeDialogOpen = true;
            dialog.Show("关闭 apitab？", "直接退出应用，还是最小化到系统托盘继续运行？", "直接关闭",
                        "最小化到托盘",
                        [=]() mutable {
                            closeDialogOpen = false;
                            closeBehavior = 1;
                            saveSessionPreference("close_behavior", "1");
                            application.Quit();
                        },
                        [=]() mutable {
                            closeDialogOpen = false;
                            closeBehavior = 2;
                            saveSessionPreference("close_behavior", "2");
                            window.Hide();
                        },
                        huxerui::DialogOptions{
                            .dismiss_on_outside_press = false,
                            .dismiss_on_cancel = false,
                        });
            return true;
        },
        0);

    huxerui::View content = huxerui::Column {
        // 自定义标题栏岛：Logo + 项目标签条 + 齿轮（框架在其右侧渲染窗口按钮）。
        huxerui::WindowTitleBar {
            LogoBadge(),
            huxerui::Row {ProjectTabStrip(tabs, activeProject)}.With(huxerui::Grow(1.0F)),
            huxerui::IconButton(app::images::gear, "全局设置")
                .OnClick([navPage] { navPage = pages::kAppSettings; })
                .With(huxerui::Tooltip("全局设置")),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(theme.spacing.medium,
                                                                  theme.spacing.extra_small)),
                  huxerui::Spacing(theme.spacing.medium),
                  huxerui::Background(theme.colors.primary)),
        huxerui::Row {
            SideShell(navPage),
            pages::PageFor(navPage.Get(), navPage, tabs, activeProject, themeMode, closeBehavior)
                .Key(navPage.Get() * 100000 + activeProject.Get()),
        }
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
                               .With(huxerui::Spacing(theme.spacing.medium));

    return dark ? huxerui::View{huxerui::MaterialDarkTheme{content}}
                : huxerui::View{huxerui::MaterialTheme{content}};
}

} // namespace apitab::ui
