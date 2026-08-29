// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘）：
//   标题栏岛：Logo(AT) + 顶级项目标签条（Grow，超长项目名裁剪）+ 齿轮(全局设置)
//     + 框架窗口按钮；底色随全局主题（surface_container_low），深色主题为
//     「AI 极客风」近黑配色（GeekDarkThemeSpec）。
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

// 「AI 极客风」深色主题：以 Material 深色令牌为底，覆盖为近黑底 +
// 电气青/亮蓝 accent 的配色；形状/间距/排版沿用内置深色方案。
huxerui::ThemeSpec GeekDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.colors.primary = huxerui::Color::Rgb(62, 198, 224);       // #3EC6E0 电气青
    spec.colors.on_primary = huxerui::Color::Rgb(6, 32, 42);       // #06202A 近黑
    spec.colors.secondary = huxerui::Color::Rgb(76, 194, 255);     // #4CC2FF 亮蓝
    spec.colors.on_secondary = huxerui::Color::Rgb(6, 32, 42);     // #06202A
    spec.colors.secondary_container = huxerui::Color::Rgb(24, 32, 44);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(230, 237, 243);
    spec.colors.background = huxerui::Color::Rgb(7, 9, 13);        // #07090D
    spec.colors.surface = huxerui::Color::Rgb(10, 13, 18);         // #0A0D12
    spec.colors.surface_container_low = huxerui::Color::Rgb(13, 17, 23);    // #0D1117
    spec.colors.surface_container = huxerui::Color::Rgb(18, 22, 30);        // #12161E
    spec.colors.surface_container_high = huxerui::Color::Rgb(24, 32, 44);   // #18202C
    spec.colors.surface_container_highest = huxerui::Color::Rgb(31, 41, 55); // #1F2937
    spec.colors.on_surface = huxerui::Color::Rgb(230, 237, 243);   // #E6EDF3
    spec.colors.on_surface_variant = huxerui::Color::Rgb(139, 148, 158); // #8B949E
    spec.colors.outline = huxerui::Color::Rgb(45, 55, 69);         // #2D3745
    spec.colors.inverse_surface = huxerui::Color::Rgb(230, 237, 243);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(10, 13, 18);
    spec.colors.error = huxerui::Color::Rgb(255, 92, 92);          // #FF5C5C
    return spec;
}

// 软件徽标：字母 AT 合成的圆角块。
[[huxerui::composable]] huxerui::View LogoBadge() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text("AT", huxerui::TextRole::Title)
        .With(huxerui::Frame{.width = 36.0F, .height = 28.0F},
              huxerui::Background(theme.colors.primary),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::Foreground(theme.colors.on_primary),
              huxerui::Align{.horizontal = huxerui::HorizontalAlignment::Center,
                             .vertical = huxerui::VerticalAlignment::Center});
}

// 单个标签页：激活态 = 最高层级容器底 + 主文字色；未激活 = 略深容器底 + 次级文字色。
// 整块外层只负责激活与切换（点击卸载内容子树，推迟出指针事件路径，CLAUDE.md 约定 6）；
// 内层用两个兄弟节点分别承载“切换”与“关闭”，避免各自做一次整标签的背景重绘。
[[huxerui::composable]] huxerui::View ProjectTab(
    huxerui::State<std::size_t> navPage, huxerui::State<std::vector<std::int64_t>> tabs,
    huxerui::State<std::int64_t> activeProject, std::int64_t id, huxerui::View leading,
    std::string name) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    const bool active = activeProject.Get() == id;

    // 标题栏底色为 surface_container_low：未激活标签用 surface_container 微微浮起，
    // 激活标签用 surface_container_highest 进一步提亮。
    const huxerui::Color tabFill =
        active ? theme.colors.surface_container_highest : theme.colors.surface_container;
    const huxerui::Color tabForeground =
        active ? theme.colors.on_surface : theme.colors.on_surface_variant;

    auto activateProject = [tasks, activeProject, tabs, navPage](std::int64_t tabId) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            if (tabId != 0) {
                g_requests.selectProject(tabId);
                g_loadtest.setProject(tabId);
                saveSessionPreference("active_project", std::to_string(tabId));
                if (navPage.Get() == pages::kHome) navPage = pages::kRequest; // 进入工作区
            } else {
                navPage = pages::kHome; // 主页标签
            }
            activeProject = tabId;
        });
    };
    auto closeTab = [tasks, activeProject, tabs, navPage](std::int64_t tabId) {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            std::vector<std::int64_t> rest = tabs.Get();
            std::erase(rest, tabId);
            tabs = rest;
            if (activeProject.Get() == tabId) {
                g_requests.selectProject(0);
                g_loadtest.setProject(0);
                saveSessionPreference("active_project", "0");
                activeProject = 0;
                navPage = pages::kHome;
            }
        });
    };

    const auto badgeFont =
        huxerui::Font::System(12.0F).WithWeight(huxerui::FontWeight::SemiBold);
    // 主页标签只放图标：省略文字、收窄边距并固定窄宽，避免挤占项目标签空间；
    // 项目标签限宽 180 并裁剪子内容，超长项目名截断显示。
    const bool iconOnly = name.empty();
    return huxerui::Row {
        // 切换区：点击 = 激活本标签。
        huxerui::Row {std::move(leading),
                      iconOnly
                          ? huxerui::View{huxerui::Spacer().With(huxerui::Frame{.width = 0.0F})}
                          : huxerui::View{huxerui::Text(name, huxerui::TextRole::Label)
                                              .Style(huxerui::TextStyle{
                                                  .font = badgeFont,
                                                  .foreground = tabForeground})}}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 4.0F)),
                  huxerui::Spacing(4.0F))
            .OnClick([activateProject, id] { activateProject(id); }),
        // 关闭区（仅项目标签）：独立兄弟，点击关闭标签页。
        id != 0
            ? huxerui::View{huxerui::Text("✕", huxerui::TextRole::Label)
                                .Style(huxerui::TextStyle{
                                    .font = badgeFont,
                                    .foreground = tabForeground,
                                })
                                .With(huxerui::Padding(4.0F))
                                .OnClick([closeTab, id] { closeTab(id); })
                                .On<huxerui::ViewEvents::PointerMove>(
                                    [](const huxerui::PointerEvent&) {})}
            : huxerui::View{huxerui::Spacer().With(huxerui::Frame{.width = 0.0F})},
    }
        .With(huxerui::Spacing(0.0F), huxerui::Background(tabFill),
              huxerui::Foreground(tabForeground),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::ClipChildren(),
              huxerui::Padding(iconOnly ? huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)
                                        : huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
              iconOnly ? huxerui::Frame{.width = 32.0F, .height = 28.0F}
                       : huxerui::Frame{.height = 28.0F, .max_width = 180.0F});
}

// 顶级标签条：第一个为主页（固定不可关，房子图标），其后每个项目一个可关标签。
[[huxerui::composable]] huxerui::View ProjectTabStrip(
    huxerui::State<std::size_t> navPage, huxerui::State<std::vector<std::int64_t>> tabs,
    huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    std::vector<std::pair<std::int64_t, std::string>> items;
    items.emplace_back(0, ""); // 主页标签：图标 only，无文字
    for (const db::Project& p : g_requests.allProjects()) {
        for (std::int64_t id : tabs.Get()) {
            if (id == p.id) items.emplace_back(p.id, p.name);
        }
    }

    std::vector<huxerui::View> chips;
    for (const auto& [id, name] : items) {
        // 主页标签只放房子图标（无文字），项目标签用项目名。
        const huxerui::View leading =
            id == 0 ? huxerui::View{huxerui::Image(app::images::home)
                                        .With(huxerui::Frame{.width = 16.0F, .height = 16.0F})}
                    : huxerui::View{huxerui::Spacer().With(huxerui::Frame{.width = 0.0F})};
        chips.push_back(ProjectTab(navPage, tabs, activeProject, id, std::move(leading), name)
                            .Key(id == 0 ? 0LL : id));
    }

    return huxerui::Row(std::move(chips))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 左列岛：图标侧边栏（选中态用实心图标变体，悬停显示文字提示）。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
    };
    const std::array<Item, 6> items{
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
        const std::size_t page = i + 1; // 侧栏项从 kRequest 开始
        const huxerui::ImageResource& icon = navPage.Get() == page ? item.icon_selected : item.icon;
        buttons.push_back(
            huxerui::IconButton(icon, item.tooltip)
                .OnClick([tasks, navPage, page] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = page;
                    });
                })
                .With(huxerui::Tooltip(item.tooltip)));
    }
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
    // IsAvailable() 内部会观察托盘可用性 State：DBus 托盘宿主就绪较晚时，
    // 这里在组合期订阅，可用性翻转后本作用域重组、托盘随后注册。
    const bool trayAvailable = tray.IsAvailable();
    auto dialog = huxerui::UseDialog();
    auto tasks = huxerui::UseTaskScope();

    // 初始值在 UseState 之前算好（组合体内不写 State）：
    // 主题模式 0=跟随系统 1=深色 2=浅色，未保存偏好时默认深色（极客风黑底）。
    int initialThemeMode = 1;
    if (sessionPreference("theme_mode") == "0") initialThemeMode = 0;
    if (sessionPreference("theme_mode") == "2") initialThemeMode = 2;
    // 关闭行为：0=每次询问 1=直接关闭 2=最小化到托盘
    int initialCloseBehavior = 0;
    if (sessionPreference("close_behavior") == "1") initialCloseBehavior = 1;
    if (sessionPreference("close_behavior") == "2") initialCloseBehavior = 2;

    auto navPage = huxerui::UseState<std::size_t>(pages::kHome);
    auto themeMode = huxerui::UseState<int>(std::move(initialThemeMode));
    // 顶级标签：activeProject = 0 为主页标签；其余值为打开的项目 id。
    auto tabs = huxerui::UseState<std::vector<std::int64_t>>({});
    auto activeProject = huxerui::UseState<std::int64_t>(0);
    auto closeBehavior = huxerui::UseState<int>(std::move(initialCloseBehavior));
    auto closeDialogOpen = huxerui::UseState(false);
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());

    // 托盘：图标 + 菜单；点击托盘图标激活主窗口。仅在可用时注册；
    // 首次组合时宿主未就绪则跳过，待可用性触发重组后再注册。
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
    // 托盘可用性在关闭时动态查询（不要用组合期快照，避免错过宿主晚就绪）。
    window.OnCloseRequest(
        [=]() mutable -> bool {
            if (!tray.IsAvailable()) return false; // 无托盘：交给系统直接关闭
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
            huxerui::Row {ProjectTabStrip(navPage, tabs, activeProject)}.With(huxerui::Grow(1.0F)),
            huxerui::IconButton(app::images::gear, "全局设置")
                .OnClick([tasks, navPage] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = pages::kAppSettings;
                    });
                })
                .With(huxerui::Tooltip("全局设置"), huxerui::Frame{.height = 28.0F}),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(theme.spacing.medium,
                                                                  theme.spacing.extra_small)),
                  huxerui::Spacing(theme.spacing.medium),
                  huxerui::Background(theme.colors.surface_container_low)),
        huxerui::Row {
            SideShell(navPage),
            pages::PageFor(navPage.Get(), navPage, tabs, activeProject, themeMode, closeBehavior)
                .Key(navPage.Get() * 100000 + activeProject.Get()),
        }
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
                               .With(huxerui::Spacing(theme.spacing.medium));

    // 深色分支用极客风自定义 spec（MaterialDarkTheme 无自定义 spec 构造，走
    // MaterialTheme(spec, content)）；浅色分支保持内置 Material 浅色。
    return dark ? huxerui::View{huxerui::MaterialTheme(GeekDarkThemeSpec(), content)}
                : huxerui::View{huxerui::MaterialTheme{content}};
}

} // namespace apitab::ui
