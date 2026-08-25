// app.cpp — apitab 薄入口。
#include <eui_neo.h>

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
        return true;
    }();
    (void)preferencesLoaded;
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
    constexpr float workspaceH = 42.0f;

    ui.stack("root")
        .size(screen.width, screen.height)
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
                .size(screen.width - kMargin * 2.0f - 32.0f, kInputHeight)
                .zIndex(40)
                .content([&] {
                    drawProjectWorkspaceBar(ui, 0, 0,
                                            screen.width - kMargin * 2.0f - 32.0f, theme);
                })
                .build();

            const bool overlayPage = isOverlayPage(g_page);
            const bool projectContext = isProjectPage(g_page) && g_activeProjectTabId != 0;
            const bool compactShell = screen.width < 860.0f;
            const bool showCollectionSidebar = projectContext && !compactShell &&
                                               (g_page == Page::Request || g_page == Page::Load);
            const bool requestTabs = showCollectionSidebar;
            const float bodyTop = workspaceH;
            const float bodyBottom = std::max(bodyTop, screen.height - 20.0f - kMargin);

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
                drawSidebar(ui, screen, bodyTop, theme);
                contentX = kRailWidth + kSidebarWidth + kMargin;
            }
            const float contentW = std::max(0.0f, screen.width - contentX - kMargin);
            float pageY = bodyTop + kMargin;
            if (showCollectionSidebar && requestTabs) {
                drawRequestTabStrip(ui, contentX, pageY, contentW, theme);
                pageY += 32.0f;
            }
            if (compactShell && projectContext && (g_page == Page::Request || g_page == Page::Load)) {
                ui.text("shell.compact.notice")
                    .position(contentX, pageY).size(contentW, 24.0f)
                    .text("请求集合侧栏已在窄窗口隐藏，可拉宽窗口查看")
                    .fontSize(kFontLabel).color(theme.hintText).build();
                pageY += 30.0f;
            }
            const float projectFooterH = projectContext ? 26.0f : 0.0f;
            const float projectFooterGap = projectContext ? kGap : 0.0f;
            const float pageH = std::max(0.0f, bodyBottom - pageY - projectFooterH - projectFooterGap);

            switch (g_page) {
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
                    drawHistoryPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::ProjectSettings:
                    if (projectContext) drawProjectSettingsPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
            }

            if (projectContext) {
                const float footerY = std::max(pageY, bodyBottom - projectFooterH);
                ui.stack("project.footer")
                    .position(contentX, footerY)
                    .size(contentW, projectFooterH)
                    .zIndex(8)
                    .content([&] {
                        ui.rect("project.footer.bg")
                            .size(contentW, projectFooterH)
                            .color(components::theme::withAlpha(theme.components.surface,
                                                                theme.dark ? 0.72f : 0.88f))
                            .border(1.0f, components::theme::withAlpha(theme.components.border, 0.55f))
                            .radius(5.0f)
                            .build();
                        components::button(ui, "project.footer.cookies")
                            .position(std::max(0.0f, contentW - 142.0f), 1.0f)
                            .size(138.0f, 24.0f)
                            .icon(0xF013).text("全局 Cookies").fontSize(kFontLabel)
                            .theme(theme.components, false)
                            .onClick([] { g_globalCookieOpen = true; })
                            .build();
                    })
                    .build();
            }

            drawStatusBar(ui, screen.width, screen.height, g_statusMessage, theme);
            if (showCollectionSidebar) {
                drawSidebarDialogs(ui, screen, theme);
            }
            if (projectContext) {
                drawRequestPageDialogs(ui, screen, theme);
            }

            if (g_confirm.open) {
                components::dialog(ui, "confirm")
                    .open(true).screen(screen.width, screen.height).size(380.0f, 160.0f)
                    .title(g_confirm.title).message(g_confirm.message)
                    .primaryText(tr(UiText::Confirm)).secondaryText(tr(UiText::Cancel)).theme(theme.components)
                    .onPrimary([] {
                        if (g_confirm.action) g_confirm.action();
                        g_confirm.open = false;
                        g_confirm.action = nullptr;
                    })
                    .onSecondary([] {
                        g_confirm.open = false;
                        g_confirm.action = nullptr;
                    })
                    .build();
            }
        })
        .build();
}

} // namespace app
