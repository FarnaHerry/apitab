// app.cpp — 应用壳（岛屿架构 + 自定义标题栏 + 托盘）：
//   标题栏岛：Logo(AT) + 顶级项目标签条（主页钉在最左，项目标签横向滚动）+ 齿轮
//     (全局设置) + 框架窗口按钮；收窄为 32px 高、去背景直接融入窗口底色，
//     主题为极简 AI 黑白风（MinimalDark/MinimalLightThemeSpec，对齐 tinynext 配色）。
//   下方：左侧图标侧边栏（无岛屿包裹，直接落在窗口背景上）｜内容区（页面自己的
//   一级岛屿划分区域，外壳不再套岛）。根节点刷整窗底色（rootSpec.colors.background——
//   AppRoot 在主题 provider 之上，UseTheme 只能拿到默认浅色 spec，须按 dark 自选）。
//   响应式：UseViewportClass() Compact 时收窄侧栏宽度与各处间距。
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
            // 主页已整宽覆盖侧栏：未打开项目时本页不可达，无需兜底。
            return RequestPage(activeProject);
        case kLoad:
            return LoadTestPage();
        // kWebSocket/kTcp 不再可达：已并入请求页内部标签（PageIndex 枚举值保留）。
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

// 标题栏内容统一高度：Logo / 主页标签 / 项目标签 / 齿轮全部 20pt，垂直居中。
// 20pt 低于系统窗口按钮条的最小高度，不再由我们的内容把整条标题栏顶高。
constexpr float kTitleBarContentHeight = 20.0F;

// 极简 AI 黑白风主题（对齐 tinynext 的 heibu geekBlack/geekWhite 配色）：
// 深色 = 近纯黑底 + 纯白主色（主色控件白底黑字）；浅色 = 近白底 + 纯黑主色。
// 文本/描边只用地道中灰，状态色仅 error 保留柔和红。
huxerui::ThemeSpec MinimalDarkThemeSpec() {
    huxerui::ThemeSpec spec = huxerui::MaterialDarkThemeSpec();
    spec.colors.primary = huxerui::Color::Rgb(255, 255, 255);      // 纯白主色
    spec.colors.on_primary = huxerui::Color::Rgb(10, 10, 12);      // 白底上翻黑
    spec.colors.secondary = huxerui::Color::Rgb(214, 214, 217);
    spec.colors.on_secondary = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.secondary_container = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.background = huxerui::Color::Rgb(10, 10, 12);      // #0A0A0C 近纯黑
    spec.colors.surface = huxerui::Color::Rgb(14, 14, 17);
    spec.colors.surface_container_low = huxerui::Color::Rgb(19, 19, 22);
    spec.colors.surface_container = huxerui::Color::Rgb(24, 24, 28);
    spec.colors.surface_container_high = huxerui::Color::Rgb(30, 30, 35);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(37, 37, 43);
    spec.colors.on_surface = huxerui::Color::Rgb(242, 242, 242);   // 0.95 白
    spec.colors.on_surface_variant = huxerui::Color::Rgb(148, 148, 153); // 0.58 灰
    spec.colors.outline = huxerui::Color::Rgb(46, 46, 52);
    spec.colors.inverse_surface = huxerui::Color::Rgb(242, 242, 242);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(10, 10, 12);
    spec.colors.error = huxerui::Color::Rgb(235, 122, 112);        // tinynext 柔和红
    return spec;
}

huxerui::ThemeSpec MinimalLightThemeSpec() {
    huxerui::ThemeSpec spec; // 默认值即内置浅色方案
    spec.colors.primary = huxerui::Color::Rgb(0, 0, 0);            // 纯黑主色
    spec.colors.on_primary = huxerui::Color::Rgb(255, 255, 255);   // 黑底上翻白
    spec.colors.secondary = huxerui::Color::Rgb(69, 69, 71);
    spec.colors.on_secondary = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.secondary_container = huxerui::Color::Rgb(229, 229, 232);
    spec.colors.on_secondary_container = huxerui::Color::Rgb(31, 31, 31);
    spec.colors.background = huxerui::Color::Rgb(244, 244, 245);   // #F4F4F5 近白
    spec.colors.surface = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container_low = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.surface_container = huxerui::Color::Rgb(240, 240, 241);
    spec.colors.surface_container_high = huxerui::Color::Rgb(231, 231, 233);
    spec.colors.surface_container_highest = huxerui::Color::Rgb(222, 222, 225);
    spec.colors.on_surface = huxerui::Color::Rgb(31, 31, 31);      // 0.12 近黑
    spec.colors.on_surface_variant = huxerui::Color::Rgb(115, 115, 120); // 0.45 灰
    spec.colors.outline = huxerui::Color::Rgb(201, 201, 204);
    spec.colors.inverse_surface = huxerui::Color::Rgb(31, 31, 31);
    spec.colors.inverse_on_surface = huxerui::Color::Rgb(255, 255, 255);
    spec.colors.error = huxerui::Color::Rgb(204, 64, 51);
    return spec;
}

// 软件徽标：字母 AT 合成的圆角块。与标题栏所有控件统一 24pt 高（kTitleBarHeight）。
[[huxerui::composable]] huxerui::View LogoBadge() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Text("AT", huxerui::TextRole::Title)
        .With(huxerui::Frame{.width = 32.0F, .height = kTitleBarContentHeight},
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

    // 标题栏已无底色（融入窗口背景）：未激活标签用 surface_container 微微浮起，
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
    // 限宽与裁剪只压内部「切换区」（图标+文字），长项目名截断而行尾 ✕ 永远完整显示。
    // 所有标签统一 kTitleBarContentHeight 高；内外两层 Row 都交叉轴居中，
    // 图标/文字/✕ 不会在 24pt 条里各自顶格漂移。
    const bool iconOnly = name.empty();
    return huxerui::Row {
        // 切换区：点击 = 激活本标签。
        huxerui::Row {std::move(leading),
                      iconOnly
                          ? huxerui::View{huxerui::Row{}}
                          : huxerui::View{huxerui::Text(name, huxerui::TextRole::Label)
                                              .Style(huxerui::TextStyle{
                                                  .font = badgeFont,
                                                  .foreground = tabForeground})}}
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                  huxerui::Spacing(4.0F), huxerui::Frame{.max_width = 140.0F},
                  huxerui::ClipChildren(),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
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
            : huxerui::View{huxerui::Row{}},
    }
        .With(huxerui::Spacing(0.0F), huxerui::Background(tabFill),
              huxerui::Foreground(tabForeground),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Padding(iconOnly ? huxerui::EdgeInsets::Symmetric(4.0F, 1.0F)
                                        : huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
              iconOnly ? huxerui::Frame{.width = 32.0F, .height = kTitleBarContentHeight}
                       : huxerui::Frame{.height = kTitleBarContentHeight});
}

// 顶级标签条：主页标签（房子图标，固定不可关）钉在最左不参与滚动；
// 其余每个打开的项目一个可关标签，放进横向 ScrollView，溢出时滚动而不是挤变形。
[[huxerui::composable]] huxerui::View ProjectTabStrip(
    huxerui::State<std::size_t> navPage, huxerui::State<std::vector<std::int64_t>> tabs,
    huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    // 主页标签：图标 only，无文字。
    huxerui::View homeTab =
        ProjectTab(navPage, tabs, activeProject, 0,
                   huxerui::View{huxerui::Image(app::images::home)
                                     .With(huxerui::Frame{.width = 16.0F, .height = 16.0F})},
                   "")
            .Key(std::int64_t{0});

    std::vector<huxerui::View> chips;
    for (const db::Project& p : g_requests.allProjects()) {
        for (std::int64_t id : tabs.Get()) {
            if (id != p.id) continue;
            chips.push_back(
                ProjectTab(navPage, tabs, activeProject, p.id,
                           huxerui::View{huxerui::Row{}},
                           p.name)
                    .Key(p.id));
        }
    }

    return huxerui::Row {
        std::move(homeTab),
        huxerui::ScrollView(huxerui::Row(std::move(chips))
                                .With(huxerui::Spacing(theme.spacing.small),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)))
            .ScrollAxis(huxerui::Axis::Horizontal)
            .With(huxerui::ScrollBar{}, huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// 左列：图标侧边栏（选中态用实心图标变体，悬停显示文字提示）。
// 去岛屿包裹：图标按钮直接落在窗口背景上；WebSocket/TCP 已并入请求页标签，不再单列。
[[huxerui::composable]] huxerui::View SideShell(huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    struct Item {
        huxerui::ImageResource icon;
        huxerui::ImageResource icon_selected;
        const char* tooltip;
        std::size_t page; // 目标页码（PageIndex），不再用下标推算
    };
    const std::array<Item, 4> items{
        Item{app::images::request, app::images::request_selected, "请求", pages::kRequest},
        Item{app::images::loadtest, app::images::loadtest_selected, "压测", pages::kLoad},
        Item{app::images::history, app::images::history_selected, "历史记录", pages::kHistory},
        Item{app::images::project_settings, app::images::project_settings_selected, "项目设置",
             pages::kProjectSettings},
    };

    std::vector<huxerui::View> buttons;
    for (const Item& item : items) {
        const std::size_t page = item.page;
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
        .With(huxerui::Padding(compact ? theme.spacing.small : theme.spacing.medium),
              huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = compact ? 44.0F : 56.0F},
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
    // 主题模式 0=跟随系统 1=深色 2=浅色，未保存偏好时默认深色（极简黑白黑底）。
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

    const bool dark =
        themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
    // 注意：AppRoot 里的 UseTheme() 拿到的是 MaterialTheme provider 之上（应用外）
    // 的默认浅色 spec——主题由本函数返回时包进子树，自身读不到。所以根节点自身的
    // 配色（整窗背景、标题栏底、间距）必须直接按 dark 选 spec；子组件在 provider
    // 之下，它们的 UseTheme() 是正常的。
    const huxerui::ThemeSpec rootSpec = dark ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
    // 响应式：Compact(<600) 收窄间距，Medium/Expanded 保持现状。
    // 外壳统一间隙（标题栏↔主行、侧栏↔内容区、根 Column 子项）：small(8pt)，
    // 岛屿间隙由各页面根同行收敛。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;
    const float gap = compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small;

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
        // 全部内容统一 20pt 高（kTitleBarContentHeight）；WindowTitleBar 构造即带
        // 交叉轴居中，这里给中间标签条包装 Row 也补上居中，任何一侧偏高都不漂移。
        huxerui::WindowTitleBar {
            LogoBadge(),
            huxerui::Row {ProjectTabStrip(navPage, tabs, activeProject)}
                .With(huxerui::Grow(1.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            // 齿轮不用 IconButton（框架内置最小触摸尺寸，Frame 压不住、比标签高
            // 一截）：裸 Image + 自绘热区，与项目标签同高同底，尺寸完全受控。
            huxerui::Row {
                huxerui::Image(app::images::gear)
                    .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
            }
                .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                      huxerui::Frame{.height = kTitleBarContentHeight},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::Background(rootSpec.colors.surface_container),
                      huxerui::CornerRadius(rootSpec.shapes.medium),
                      huxerui::Tooltip("全局设置"))
                .OnClick([tasks, navPage] {
                    // 切页会卸载内容子树：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        navPage = pages::kAppSettings;
                    });
                }),
        }
            // 标签栏收窄 + 去背景：直接融入窗口底色，不再垫 surface_container_low。
            // 垂直零内边距：内容本身 24pt 高，与 title_bar_height 对齐，避免
            // 标题栏下缘与岛屿之间多出一条空隙。
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(
                      compact ? rootSpec.spacing.extra_small : rootSpec.spacing.small, 0.0F)),
                  huxerui::Spacing(gap)),
        // 主行：侧栏（无岛屿包裹）+ 内容区；Grow 吃满标题栏之外的剩余高度。
        // 内容区不再套外壳岛：区域划分由各页面自己的一级岛屿承担，避免双层嵌套。
        // 主页与全局设置（都与项目无关）内容整宽覆盖侧栏：未打开项目就点不到任何
        // 项目相关入口。占位必须用空 Row——Spacer 自带 Grow(1)，会分走一半宽度。
        huxerui::Row {
            navPage.Get() == pages::kHome || navPage.Get() == pages::kAppSettings
                ? huxerui::View{huxerui::Row{}}
                : SideShell(navPage),
            pages::PageFor(navPage.Get(), navPage, tabs, activeProject, themeMode, closeBehavior)
                .Key(navPage.Get() * 100000 + activeProject.Get())
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(gap),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Grow(1.0F)),
    }
                               .With(huxerui::Spacing(gap),
                                     // 窗口整体背景：主题背景色刷满根节点，
                                     // 否则深色模式下岛间缝隙透出窗口默认白底。
                                     huxerui::Background(rootSpec.colors.background),
                                     // 交叉轴必须 Stretch：否则主行（侧栏+页面区）按内容
                                     // 收缩成内容宽，页面里所有 Grow 失去上界、岛屿无法
                                     // 占满逻辑区块（首页整体漂移/右对齐的根因）。
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 两个分支都走 MaterialTheme(spec, content) 自定义 spec：极简黑白双主题。
    return dark ? huxerui::View{huxerui::MaterialTheme(MinimalDarkThemeSpec(), content)}
                : huxerui::View{huxerui::MaterialTheme(MinimalLightThemeSpec(), content)};
}

} // namespace apitab::ui
