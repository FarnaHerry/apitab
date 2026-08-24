// app.cpp — apitab 薄入口。
//
// compat.eui-neo 的 `app-main` 特性提供 main()（core/app/glfw_app_main.cpp）；
// 本 TU 只定义每个 EUI 应用必须提供的两个符号 —— app::dslAppConfig() 与
// app::compose() —— 并把绘制分派给 apitab.ui.* 模块：
//
//   apitab.utils              纯 string/number 帮助函数（无 UI 依赖）
//   apitab.config             数据目录 / k6 二进制解析（无 UI 依赖）
//   apitab.api_engine         引擎抽象（RequestSpec / ResponseView / LoadSummary）
//   apitab.curl_engine        单次请求引擎（libcurl，工作线程）
//   apitab.k6_engine          压测引擎（k6 外部进程）
//   apitab.db                 SQLite 持久化（requests / history / load_tests）
//   apitab.store.requests     领域 store：集合 + 发送 + 历史（g_requests，自保留引擎）
//   apitab.store.loadtest     领域 store：压测（g_loadtest，自保留引擎）
//   apitab.store.ui           视图 store：草稿 / 页面 / 响应 / 状态消息（无 eui）
//   apitab.ui.*               主题 / 控件 / 侧栏 / 请求页 / 压测页 / 历史页
//
// 事件驱动（对齐 tinynext 纪律）：compose 不挂 onFrame —— eui 会把挂 onFrame
// 的元素当成「每帧都在动」而强制满帧重绘。引擎完成时经 requestUiUpdate() 唤醒
// 一帧，这里 poll*/drain* 取回结果写入视图 store；空闲时 UI 走 glfwWaitEvents
// 睡眠，零渲染零 CPU。
#include <eui_neo.h>

import std;
import apitab.api_engine;
import apitab.store.requests;
import apitab.store.loadtest;
import apitab.store.ui;
import apitab.ui.utils;
import apitab.ui.theme;
import apitab.ui.widgets;
import apitab.ui.sidebar;
import apitab.ui.topbars;
import apitab.ui.request_page;
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
        // 窗口物理尺寸 = 设计尺寸 × kUI（eui 按物理像素建窗，不自动乘 uiScale）。
        .windowSize(static_cast<int>(1180.0f * kUI), static_cast<int>(760.0f * kUI))
        // 最大帧率 0 = 跟随显示器刷新率。
        .fps(0.0)
        .showDebugStatsInTitle(false)
        .textFont("NotoSansSC-Regular.ttf")
        .iconFont("FontAwesome7.otf");
    return config;
}

void compose(eui::Ui& ui, const eui::Screen& screen) {
    // ---- 后台 → UI 的结果取回（引擎完成时 requestUiUpdate 唤醒本帧）----

    // 单次请求结果：写回发起 tab（落库历史已在 store 内完成）。
    api::ResponseView resp;
    if (g_requests.pollResult(resp)) {
        RequestTab& tab = [&]() -> RequestTab& {
            if (RequestTab* t = findTab(g_sendingTabUid)) return *t;
            return activeTab();  // 发起 tab 已被关闭 → 给当前 tab 兜底展示
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

    // 压测输出与汇总。
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

    // 后台线程投递的状态消息（保留扩展点；当前引擎错误直接走 poll* 路径）。
    for (std::string& msg : drainStatus()) {
        showStatus(std::move(msg));
    }

    const AppTheme& theme = currentTheme();

    // ---- 布局 ----
    // 根 stack：底层全屏主题背景（clearColor 初始化时固化，主题色靠重绘覆盖），
    // 其余按绝对定位排布 —— 与 tinynext 同法，完全可控、随窗口自适应。
    ui.stack("root")
        .size(screen.width, screen.height)
        .content([&] {
            ui.rect("theme.background")
                .position(0, 0)
                .size(screen.width, screen.height)
                .color(theme.components.background)
                .build();

            // ===================== 图标导航栏 =====================
            ui.stack("rail")
                .position(0, 0)
                .size(kRailWidth, screen.height)
                .zIndex(5)
                .content([&] {
                    ui.rect("rail.logo.bg")
                        .position((kRailWidth - 18.0f) * 0.5f, 10.0f)
                        .size(18.0f, 18.0f)
                        .color(theme.components.primary)
                        .radius(5.0f)
                        .build();
                    ui.text("rail.logo")
                        .position(0, 10.0f)
                        .size(kRailWidth, 18.0f)
                        .text("AT")
                        .fontSize(8.0f)
                        .lineHeight(18.0f)
                        .color(theme.dark ? theme.components.surface
                                          : theme.components.background)
                        .horizontalAlign(core::HorizontalAlign::Center)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();

                    float railY = 40.0f;
                    drawRailItem(ui, "nav.home", railY, kRailWidth, 0xF015,
                                 g_page == Page::Home, theme,
                                 [] { g_page = Page::Home; });
                    railY += 30.0f;
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

                    // 基础设置入口在顶部工作区栏右侧，rail 只保留页面导航。
                })
                .build();

            // ===================== 集合侧栏 =====================
            const bool projectPage = (g_page == Page::Request || g_page == Page::Load) &&
                                     g_activeProjectTabId != 0;
            float contentX = kRailWidth + kMargin;
            if (projectPage) {
                drawSidebar(ui, screen, theme);
                contentX = kRailWidth + kSidebarWidth + kMargin;
            }

            // ===================== 内容区 =====================
            const float contentW = screen.width - contentX - kMargin;
            const float contentH = screen.height - 20.0f - kMargin * 2.0f;

            float pageY = kMargin;
            if (projectPage) {
                drawProjectWorkspaceBar(ui, contentX, pageY, contentW, theme);
                drawRequestTabStrip(ui, contentX, pageY + 32.0f, contentW, theme);
                pageY += 64.0f;
            }
            const float pageH = contentH - (pageY - kMargin);

            switch (g_page) {
                case Page::Home:
                    drawHomePage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::Request:
                    if (projectPage) drawRequestPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::Load:
                    if (projectPage) drawLoadPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
                case Page::History:
                    drawHistoryPage(ui, contentX, pageY, contentW, pageH, theme);
                    break;
            }

            // ===================== 底部状态条 =====================
            drawStatusBar(ui, screen.width, screen.height, g_statusMessage, theme);

            // ===================== 侧栏弹窗（新建分组等）=====================
            drawSidebarDialogs(ui, screen, theme);

            // ===================== 请求页弹窗（环境管理）=====================
            drawRequestPageDialogs(ui, screen, theme);

            // ===================== 全局设置弹窗 =====================
            drawSettingsDialog(ui, screen, theme);

            // ===================== 确认弹窗 =====================
            if (g_confirm.open) {
                components::dialog(ui, "confirm")
                    .open(true)
                    .screen(screen.width, screen.height)
                    .size(380.0f, 160.0f)
                    .title(g_confirm.title)
                    .message(g_confirm.message)
                    .primaryText("确定")
                    .secondaryText("取消")
                    .theme(theme.components)
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
