// app.cpp — apitab 薄入口。
#include <eui_neo.h>

#include <string.h>

#ifdef __linux__
#include <dlfcn.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

import std;
import apitab.api_engine;
import apitab.i18n;
import apitab.store.requests;
import apitab.store.loadtest;
import apitab.store.tcp;
import apitab.store.websocket;
import apitab.store.ui;
import apitab.ui.utils;
import apitab.ui.theme;
import apitab.ui.widgets;
import apitab.ui.sidebar;
import apitab.ui.topbars;
import apitab.ui.request_page;
import apitab.ui.tcp_page;
import apitab.ui.websocket_page;
import apitab.ui.loadtest_page;
import apitab.ui.history_page;
import apitab.ui.home_page;
import apitab.ui.settings_page;

namespace app {

namespace {

#ifdef __linux__
// EUI 0.5.7 的 DslAppConfig 没有最小窗口尺寸，窄窗口会触发各页换行布局
// （WS 工具栏阈值 444 内容宽 + 壳层 rail40+sidebar190+margin16 = 690 逻辑宽）。
// 这里用 X11 WM_NORMAL_HINTS 给主窗口设最小宽度（KWin/XWayland 会遵守，
// 包括交互拖拽与程序化 resize）。libX11 是 GLFW 3.4 运行时 dlopen 加载的，
// 不在链接行上，所以同样走 dlopen/dlsym 取函数，不引入新的链接依赖。
// 非 X11 后端（纯 Wayland）下找不到窗口，静默跳过。
// TODO(upstream-eui): 作者已确认最小窗口尺寸后续进 DslAppConfig —— 每次升级
// EUI 版本时 grep 新版 dsl_app.h 是否有 minWindowSize/minimumSize 之类的接口，
// 有了就删掉这整块 X11 workaround（详见 docs/eui-neo-compat.md）。
constexpr float kMinWindowWidthLogical = 700.0f;

struct X11Fns {
    decltype(&XOpenDisplay) openDisplay = nullptr;
    decltype(&XCloseDisplay) closeDisplay = nullptr;
    decltype(&XQueryTree) queryTree = nullptr;
    decltype(&XGetWindowProperty) getWindowProperty = nullptr;
    decltype(&XGetWindowAttributes) getWindowAttributes = nullptr;
    decltype(&XInternAtom) internAtom = nullptr;
    decltype(&XSetWMNormalHints) setWMNormalHints = nullptr;
    decltype(&XResizeWindow) resizeWindow = nullptr;
    decltype(&XFlush) flush = nullptr;
    decltype(&XFree) free = nullptr;
};

Window findWindowByTitle(Display* dpy, Window root, const X11Fns& X,
                         Atom netWmName, Atom utf8, const std::string& title,
                         int depth) {
    if (depth > 4) return None;
    Window rootOut = None, parentOut = None;
    Window* children = nullptr;
    unsigned count = 0;
    if (X.queryTree(dpy, root, &rootOut, &parentOut, &children, &count) == 0) return None;
    Window found = None;
    for (unsigned i = 0; i < count && found == None; ++i) {
        Atom actual = None;
        int format = 0;
        unsigned long n = 0, after = 0;
        unsigned char* value = nullptr;
        const int ok = X.getWindowProperty(dpy, children[i], netWmName, 0, 256, False,
                                           AnyPropertyType, &actual, &format, &n, &after, &value);
        if (ok == Success && value != nullptr) {
            while (n > 0 && value[n - 1] == 0) --n;  // 去掉可能的结尾 NUL
            if ((actual == utf8 || actual == XA_STRING) &&
                n == title.size() && memcmp(value, title.data(), n) == 0) {
                found = children[i];
            }
            X.free(value);
        }
        if (found == None) {
            found = findWindowByTitle(dpy, children[i], X, netWmName, utf8, title, depth + 1);
        }
    }
    if (children != nullptr) X.free(children);
    return found;
}

// 返回 true 表示已成功设置（或非 X11 环境无需重试）；false 表示窗口还没找到，下帧重试。
bool applyMinWindowSize(float logicalWidth, const std::string& title) {
    void* lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    if (lib == nullptr) lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) return true;    X11Fns X;
    X.openDisplay = reinterpret_cast<decltype(X.openDisplay)>(dlsym(lib, "XOpenDisplay"));
    X.closeDisplay = reinterpret_cast<decltype(X.closeDisplay)>(dlsym(lib, "XCloseDisplay"));
    X.queryTree = reinterpret_cast<decltype(X.queryTree)>(dlsym(lib, "XQueryTree"));
    X.getWindowProperty = reinterpret_cast<decltype(X.getWindowProperty)>(dlsym(lib, "XGetWindowProperty"));
    X.getWindowAttributes = reinterpret_cast<decltype(X.getWindowAttributes)>(dlsym(lib, "XGetWindowAttributes"));
    X.internAtom = reinterpret_cast<decltype(X.internAtom)>(dlsym(lib, "XInternAtom"));
    X.setWMNormalHints = reinterpret_cast<decltype(X.setWMNormalHints)>(dlsym(lib, "XSetWMNormalHints"));
    X.resizeWindow = reinterpret_cast<decltype(X.resizeWindow)>(dlsym(lib, "XResizeWindow"));
    X.flush = reinterpret_cast<decltype(X.flush)>(dlsym(lib, "XFlush"));
    X.free = reinterpret_cast<decltype(X.free)>(dlsym(lib, "XFree"));
    if (!X.openDisplay || !X.closeDisplay || !X.queryTree || !X.getWindowProperty ||
        !X.getWindowAttributes || !X.internAtom || !X.setWMNormalHints ||
        !X.resizeWindow || !X.flush || !X.free) {
        return true;
    }
    Display* dpy = X.openDisplay(nullptr);
    if (dpy == nullptr) return true;
    const Atom netWmName = X.internAtom(dpy, "_NET_WM_NAME", True);
    const Atom utf8 = X.internAtom(dpy, "UTF8_STRING", True);
    const Window win = findWindowByTitle(dpy, DefaultRootWindow(dpy), X,
                                         netWmName, utf8, title, 0);
    bool applied = false;
    if (win != None && logicalWidth > 0.0f) {
        XWindowAttributes attrs{};
        if (X.getWindowAttributes(dpy, win, &attrs) != 0 && attrs.width > 0) {
            // X11 像素与逻辑像素的比例在运行时实测（XWayland 可能被合成器缩放），
            // 不硬编码 kUI 或合成器缩放系数。
            const float scale = static_cast<float>(attrs.width) / logicalWidth;
            XSizeHints hints{};
            hints.flags = PMinSize;
            hints.min_width = static_cast<int>(kMinWindowWidthLogical * scale + 0.5f);
            hints.min_height = 1;  // 只约束宽度；高度方向页面内部滚动兜底
            X.setWMNormalHints(dpy, win, &hints);
            if (attrs.width < hints.min_width) {
                X.resizeWindow(dpy, win, static_cast<unsigned>(hints.min_width),
                               static_cast<unsigned>(attrs.height));
            }
            X.flush(dpy);
            applied = true;
        }
    }
    X.closeDisplay(dpy);
    return applied;
}
#endif

#ifndef __linux__
bool applyMinWindowSize(float, const std::string&) {
    return true;
}
#endif

} // namespace

const DslAppConfig& dslAppConfig() {
    static const DslAppConfig config = DslAppConfig{}
        .title("apitab — API 测试与压测")
        .pageId("apitab")
        .clearColor({0.04f, 0.04f, 0.05f, 1.0f})
        .uiScale(kUI)
        .windowSize(static_cast<int>(1180.0f * kUI), static_cast<int>(760.0f * kUI))
        .fps(0.0)
        .showDebugStatsInTitle(false)
        .textFont("NotoSansSC-Regular.ttf")
        .iconFont("FontAwesome7.otf");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    static const bool preferencesLoaded = [] {
        loadLanguagePreference();
        g_themeMode = static_cast<ThemeMode>(std::clamp(g_savedThemeMode, 0, 2));
        if (g_themeMode == ThemeMode::System) g_dark = systemDark();
        const auto openProjects = parseIdList(sessionPreference("open_projects"));
        const auto projects = g_requests.allProjects();
        for (const auto projectId : openProjects) {
            for (const auto& project : projects) {
                if (project.id == projectId) {
                    openProjectWorkspace(project.orgId, project.id);
                    break;
                }
            }
        }
        try {
            const auto activeProject = std::stoll(sessionPreference("active_project"));
            for (const auto& project : projects) {
                if (project.id == activeProject) {
                    openProjectWorkspace(project.orgId, project.id);
                    break;
                }
            }
        } catch (...) {}
        // 页面恢复放在工作区恢复之后：openProjectWorkspace 恒切到 Request 页，
        // 先恢复页面会被覆盖，History/ProjectSettings 等页永远恢复不出来。
        restoreSessionState();
        return true;
    }();
    (void)preferencesLoaded;
    // 最小窗口宽度：首帧窗口可能尚未映射，找不到就在下个事件帧重试，
    // 成功后只跑一次（重试成本 = 一次 dlopen(NOLOAD) + 窗口树遍历，仅事件帧发生）。
    static bool minWindowApplied = false;
    if (!minWindowApplied) {
        minWindowApplied = applyMinWindowSize(screen.width, dslAppConfig().titleValue);
    }
    api::ResponseView resp;
    if (g_requests.pollResult(resp)) {
        RequestTab& tab = [&]() -> RequestTab& {
            if (RequestTab* t = findTab(g_sendingTabUid)) return *t;
            return activeTab();
        }();
        tab.response = std::move(resp);
        tab.hasResponse = true;
        tab.bodyScroll = 0.0f;
        markHistoryDirty();
        showStatus(tab.response.ok
                       ? std::format("{}  ·  {}", tab.response.status,
                                     formatMs(tab.response.totalMs))
                       : ("请求失败: " + tab.response.error));
    }

    appendLoadOutput(g_loadtest.drainOutput());
    api::LoadSummary summary;
    if (g_loadtest.pollSummary(summary)) {
        g_loadSummary = std::move(summary);
        g_hasLoadSummary = true;
        g_showLoadRecords = false;
        markLoadRecordsDirty();
        showStatus(g_loadSummary.ok
                       ? std::format("压测完成: {} 请求 · RPS {:.0f} · P95 {}",
                                     g_loadSummary.requests, g_loadSummary.rps,
                                     formatMs(g_loadSummary.p95Ms))
                       : ("压测异常: " + g_loadSummary.error));
    }
    for (std::string& msg : drainStatus()) showStatus(std::move(msg));
    if (g_activeProjectTabId != 0) persistSessionState();

    for (RequestTab& tab : g_tabs) {
        if (tab.draft.kind == api::RequestKind::WebSocket) {
            tab.wsState = g_websocket.state(tab.uid);
            std::vector<api::WebSocketEvent> events = g_websocket.drain(tab.uid);
            if (!events.empty()) {
                tab.wsEvents.insert(tab.wsEvents.end(), std::make_move_iterator(events.begin()),
                                    std::make_move_iterator(events.end()));
                constexpr std::size_t kWebSocketEventCap = 2000;
                if (tab.wsEvents.size() > kWebSocketEventCap) {
                    tab.wsEvents.erase(tab.wsEvents.begin(),
                                       tab.wsEvents.end() - static_cast<std::ptrdiff_t>(kWebSocketEventCap));
                }
            }
        } else if (tab.draft.kind == api::RequestKind::Tcp) {
            tab.tcpState = g_tcp.state(tab.uid);
            std::vector<api::TcpEvent> events = g_tcp.drain(tab.uid);
            if (!events.empty()) {
                tab.tcpEvents.insert(tab.tcpEvents.end(), std::make_move_iterator(events.begin()),
                                     std::make_move_iterator(events.end()));
                constexpr std::size_t kTcpEventCap = 2000;
                if (tab.tcpEvents.size() > kTcpEventCap) {
                    tab.tcpEvents.erase(tab.tcpEvents.begin(),
                                        tab.tcpEvents.end() - static_cast<std::ptrdiff_t>(kTcpEventCap));
                }
            }
        }
    }

    if (isProjectPage(g_page) && g_activeProjectTabId == 0) g_page = Page::Home;
    const AppTheme& theme = currentTheme();
    beginSelectionPopupFrame();
    constexpr float workspaceH = 42.0f;

    ui.stack("root")
        .size(screen.width, screen.height)
        .clip()
        .content([&] {
            ui.rect("theme.background")
                .position(0, 0).size(screen.width, screen.height)
                .color(theme.components.background).build();

            // 应用标识与工作区同属顶层；rail 仅保留项目内导航。
            ui.rect("app.logo.bg")
                .position(kMargin, kMargin)
                .size(26.0f, kInputHeight)
                .color(theme.components.primary)
                .radius(6.0f)
                .zIndex(41)
                .build();
            ui.text("app.logo")
                .position(kMargin, kMargin)
                .size(26.0f, kInputHeight)
                .text("AT")
                .fontSize(9.0f)
                .lineHeight(kInputHeight)
                .color(theme.dark ? theme.components.surface : theme.components.background)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .zIndex(42)
                .build();
            ui.stack("workspace.bar.wrap")
                .position(kMargin + 32.0f, kMargin)
                .size(nonNegative(screen.width - kMargin * 2.0f - 32.0f), kInputHeight)
                .zIndex(40)
                .content([&] {
                    drawProjectWorkspaceBar(ui, 0, 0,
                                            nonNegative(screen.width - kMargin * 2.0f - 32.0f), theme);
                })
                .build();

            const bool overlayPage = isOverlayPage(g_page);
            const bool projectContext = isProjectPage(g_page) && g_activeProjectTabId != 0;
            const bool showCollectionSidebar = projectContext &&
                                               (g_page == Page::Request || g_page == Page::Load);
            const bool requestTabs = showCollectionSidebar;
            const float bodyTop = workspaceH;
            const float bodyBottom = std::max(bodyTop, screen.height - kMargin -
                                                       (projectContext ? kIslandVInset : 0.0f));
            // 侧栏宽优先 kSidebarWidth；shell 太窄时收缩侧栏（至少给内容区留 96），
            // 不再固定 190 把内容区压成负宽。上下边与 shell 岛对齐。
            const float shellW = std::max(0.0f, screen.width - kRailWidth - kRightMargin);
            const float sidebarW = showCollectionSidebar
                ? std::min(kSidebarWidth, nonNegative(shellW - 96.0f)) : 0.0f;

            if (projectContext) {
                const float shellY = bodyTop + kIslandVInset;
                const float shellH = std::max(0.0f, bodyBottom - shellY);
                const float shellX = static_cast<float>(kRailWidth);
                if (showCollectionSidebar) {
                    drawIslandPanel(ui, "project.sidebar.island", shellX, shellY,
                                    sidebarW, shellH, theme, theme.dark ? 0.56f : 0.78f);
                    drawIslandPanel(ui, "project.content.island",
                                    shellX + sidebarW + kIslandGap, shellY,
                                    std::max(0.0f, shellW - sidebarW - kIslandGap), shellH,
                                    theme, theme.dark ? 0.62f : 0.84f);
                }
            }

            // Home 与全局设置覆盖最高级 rail 和项目侧栏。
            if (!overlayPage) {
                ui.stack("rail")
                    .position(0, bodyTop)
                    .size(kRailWidth, std::max(0.0f, bodyBottom - bodyTop + kMargin))
                    .zIndex(5)
                    .content([&] {
                        float railY = 10.0f;
                        drawRailItem(ui, "nav.request", railY, kRailWidth, 0xF1D8,
                                     g_page == Page::Request, theme,
                                     [] {
                                         if (g_activeProjectTabId == 0) showStatus("请先在主页面打开项目");
                                         else g_page = Page::Request;
                                     });
                        railY += 30.0f;
                        drawRailItem(ui, "nav.load", railY, kRailWidth, 0xF0E7,
                                     g_page == Page::Load, theme,
                                     [] {
                                         if (g_activeProjectTabId == 0) showStatus("请先在主页面打开项目");
                                         else g_page = Page::Load;
                                     });
                        railY += 30.0f;
                        drawRailItem(ui, "nav.history", railY, kRailWidth, 0xF1DA,
                                     g_page == Page::History, theme,
                                     [] { g_page = Page::History; });
                        railY += 30.0f;
                        drawRailItem(ui, "nav.project.settings", railY, kRailWidth, 0xF013,
                                     g_page == Page::ProjectSettings, theme,
                                     [] {
                                         if (g_activeProjectTabId == 0) showStatus("请先在主页面打开项目");
                                         else g_page = Page::ProjectSettings;
                                     });
                    })
                    .build();
            }

            float contentX = overlayPage ? kMargin : kRailWidth + kMargin;
            if (showCollectionSidebar) {
                // 侧栏宽优先 kSidebarWidth；shell 太窄时收缩侧栏（至少给内容区留 96），
                // 不再固定 190 把内容区压成负宽。上下边与 shell 岛对齐。
                drawSidebar(ui, screen, bodyTop + kIslandVInset, bodyBottom, sidebarW, theme);
                contentX = kRailWidth + sidebarW + kMargin;
            }
            const float contentW = std::max(0.0f, screen.width - contentX - kMargin);
            float pageY = bodyTop + kMargin;
            if (showCollectionSidebar && requestTabs) {
                drawRequestTabStrip(ui, contentX, pageY, contentW, theme);
                pageY += 32.0f;
            }
            const bool compactShell = screen.width < 860.0f;
            if (compactShell && showCollectionSidebar) {
                ui.text("shell.compact.notice")
                    .position(contentX, pageY).size(contentW, 24.0f)
                    .text("窄窗口下请求集合保持显示，右侧内容区收窄")
                    .fontSize(kFontLabel).color(theme.hintText).build();
                pageY += 30.0f;
            }
            const float pageH = std::max(0.0f, bodyBottom - pageY);

            // 页面区域被压没（极窄/极矮窗口）时跳过分页绘制，避免负几何控件。
            if (contentW > 0.0f && pageH > 0.0f) switch (g_page) {
                case Page::Home:
                    drawHomePage(ui, screen, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::GlobalSettings:
                    drawGlobalSettingsPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::Request:
                    if (projectContext) {
                        switch (activeTab().draft.kind) {
                            case api::RequestKind::Http:
                                drawRequestPage(ui, contentX, pageY, contentW, pageH, theme);
                                break;
                            case api::RequestKind::WebSocket:
                                drawWebSocketPage(ui, contentX, pageY, contentW, pageH, theme);
                                break;
                            case api::RequestKind::Tcp:
                                drawTcpPage(ui, contentX, pageY, contentW, pageH, theme);
                                break;
                        }
                    }
                    break;
                case Page::Load:
                    if (projectContext) drawLoadPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::History:
                    drawHistoryPage(ui, screen, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::ProjectSettings:
                    if (projectContext) drawProjectSettingsPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
            }

            if (showCollectionSidebar) {
                drawSidebarDialogs(ui, screen, theme);
            }
            if (projectContext) {
                drawRequestPageDialogs(ui, screen, theme);
            }

            if (g_confirm.open) {
                drawConfirmDialog(ui, screen, theme, "confirm",
                                  g_confirm.title, g_confirm.message, tr(UiText::Confirm),
                                  [] {
                                      g_confirm.open = false;
                                      g_confirm.action = nullptr;
                                  },
                                  [] {
                                      if (g_confirm.action) g_confirm.action();
                                      g_confirm.open = false;
                                      g_confirm.action = nullptr;
                                  });
            }
            drawSelectionPopupDismissLayer(ui, screen);
        })
        .build();
}

} // namespace app
