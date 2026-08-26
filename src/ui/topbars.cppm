// ui/topbars.cppm — 项目工作区标签与项目内请求标签。
module;

#include "eui_ui.h"

export module apitab.ui.topbars;

import std;
import apitab.api_engine;
import apitab.db;
import apitab.i18n;
import apitab.store.requests;
import apitab.store.tcp;
import apitab.store.websocket;
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

bool g_topbarEnvOpen = false;

struct HorizontalStripState {
    float offset = 0.0f;
    float maxOffset = 0.0f;
};

HorizontalStripState g_workspaceScroll;
HorizontalStripState g_requestScroll;

std::string orgName(std::int64_t orgId) {
    for (const auto& org : g_requests.orgs()) {
        if (org.id == orgId) return org.name;
    }
    return "未知组织";
}

const db::Project* findProject(const std::vector<db::Project>& projects, std::int64_t id) {
    for (const auto& project : projects) {
        if (project.id == id) return &project;
    }
    return nullptr;
}

void drawIconBtn(eui::Ui& ui, const std::string& id, float x, float y, unsigned int icon,
                 const AppTheme& theme, std::function<void()> onClick) {
    components::button(ui, id)
        .position(x, y)
        .size(22.0f, 22.0f)
        .icon(icon)
        .text("")
        .iconSize(10.0f)
        .theme(theme.components, false)
        .radius(kIconButtonRadius)
        .onClick(std::move(onClick))
        .build();
}

void releaseLiveRequestSessions() {
    for (const RequestTab& tab : g_tabs) {
        g_websocket.release(tab.uid);
        g_tcp.release(tab.uid);
    }
}

} // namespace

// 激活项目工作区：请求标签按项目暂存/恢复。
export void openProjectWorkspace(std::int64_t orgId, std::int64_t projectId) {
    if (projectId == 0) return;
    if (g_activeProjectTabId == projectId && g_requests.currentProjectId() == projectId) {
        g_page = Page::Request;
        return;
    }
    if (g_requests.currentProjectId() != 0) {
        releaseLiveRequestSessions();
        stashTabs(g_requests.currentProjectId());
    }
    if (const std::string err = g_requests.selectProjectInOrg(orgId, projectId); !err.empty()) {
        showStatus("切换项目失败: " + err);
        return;
    }
    if (!std::ranges::contains(g_openProjectIds, projectId)) g_openProjectIds.push_back(projectId);
    g_activeProjectTabId = projectId;
    restoreTabs(projectId);
    g_page = Page::Request;
    saveSessionPreference("active_project", std::to_string(projectId));
    saveSessionPreference("open_projects", serializeIdList(g_openProjectIds));
}

export void closeProjectWorkspace(std::int64_t projectId) {
    const auto it = std::ranges::find(g_openProjectIds, projectId);
    if (it == g_openProjectIds.end()) return;
    const bool wasActive = g_activeProjectTabId == projectId;
    if (wasActive) releaseLiveRequestSessions();
    const std::size_t index = static_cast<std::size_t>(it - g_openProjectIds.begin());
    g_openProjectIds.erase(it);
    if (!wasActive) return;
    if (g_openProjectIds.empty()) {
        g_activeProjectTabId = 0;
        g_page = Page::Home;
        return;
    }
    const std::vector<db::Project> all = g_requests.allProjects();
    const std::int64_t nextId = g_openProjectIds[std::min(index, g_openProjectIds.size() - 1)];
    if (const db::Project* next = findProject(all, nextId)) {
        openProjectWorkspace(next->orgId, next->id);
    } else {
        g_activeProjectTabId = 0;
        g_page = Page::Home;
    }
}

export void forgetProjectWorkspace(std::int64_t projectId) {
    if (g_activeProjectTabId == projectId) releaseLiveRequestSessions();
    discardStash(projectId);
    std::erase(g_openProjectIds, projectId);
    if (g_activeProjectTabId == projectId) {
        g_activeProjectTabId = 0;
        if (g_openProjectIds.empty()) {
            g_page = Page::Home;
            return;
        }
        const std::vector<db::Project> all = g_requests.allProjects();
        if (const db::Project* next = findProject(all, g_openProjectIds.front())) {
            openProjectWorkspace(next->orgId, next->id);
        } else {
            g_page = Page::Home;
        }
    }
}

// 单行项目工作区：固定主页面标签、已打开项目标签与基础设置。
// 环境选择位于项目内部请求标签条最右侧。
export void drawProjectWorkspaceBar(eui::Ui& ui, float x, float y, float w,
                                    const AppTheme& theme) {
    const auto& tokens = theme.components;
    // 顶部工作区导航是轻量岛卡：不参与项目内容的滚动，只提供稳定的
    // Home / 项目切换 / 全局设置层级。岛屿精确覆盖内容 bounds，不外扩。
    drawIslandPanel(ui, "workspace.island", x, y, w, kInputHeight,
                    theme, theme.dark ? 0.58f : 0.78f);
    const float homeW = 76.0f;
    const float settingsW = 26.0f;
    const float tabsRight = x + w - settingsW;

    ui.rect("workspace.home.hit")
        .position(x, y)
        .size(homeW, kInputHeight)
        .states(g_page == Page::Home ? components::theme::withAlpha(tokens.primary, 0.20f)
                                     : components::theme::withAlpha(tokens.surface, 0.5f),
                tokens.surfaceHover, tokens.surfaceActive)
        .radius(6.0f)
        .onClick([] { g_page = Page::Home; })
        .build();
    ui.text("workspace.home.label")
        .position(x, y)
        .size(homeW, kInputHeight)
        .icon(0xF015)
        .fontSize(10.0f)
        .lineHeight(kInputHeight)
        .color(g_page == Page::Home ? theme.titleText : theme.metaText)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    const std::vector<db::Project> all = g_requests.allProjects();
    const int count = static_cast<int>(g_openProjectIds.size());
    const float viewportX = x + homeW + kGap;
    const float viewportW = std::max(0.0f, tabsRight - viewportX);
    const float tabW = count > 0 ? std::clamp(viewportW / count, 108.0f, 190.0f) : 108.0f;
    const float totalW = std::max(0.0f, tabW * static_cast<float>(count));
    auto& scroll = g_workspaceScroll;
    scroll.maxOffset = std::max(0.0f, totalW - viewportW);
    scroll.offset = std::clamp(scroll.offset, 0.0f, scroll.maxOffset);

    ui.stack("workspace.projects.viewport")
        .position(viewportX, y).size(viewportW, kInputHeight).clip().zIndex(20)
        .content([&] {
            components::mouseArea(ui, "workspace.projects.wheel")
                .size(viewportW, kInputHeight).scrollStep(36.0f).maxScrollStep(4.0f)
                .onScroll([](const components::MouseScrollEvent& event) {
                    auto& state = g_workspaceScroll;
                    const float delta = std::abs(event.deltaX) > 0.01f ? event.deltaX : -event.deltaY;
                    state.offset = std::clamp(state.offset - delta * 36.0f, 0.0f, state.maxOffset);
                }).build();
            ui.stack("workspace.projects.content")
                .size(totalW, kInputHeight).translateX(-scroll.offset).transformedHitTest()
                .content([&] {
                    float tx = 0.0f;
                    for (const std::int64_t id : g_openProjectIds) {
                        const db::Project* project = findProject(all, id);
                        if (!project) { tx += tabW; continue; }
                        const bool active = isProjectPage(g_page) && id == g_activeProjectTabId;
                        const std::string tabId = "workspace.project." + std::to_string(id);
                        const float tabWidth = std::max(40.0f, tabW - 3.0f);
                        ui.stack(tabId).position(tx, 0).size(tabWidth, kInputHeight).content([&] {
                            ui.rect(tabId + ".hit").size(tabWidth, kInputHeight)
                                .states(active ? components::theme::withAlpha(tokens.primary, 0.20f)
                                               : components::theme::withAlpha(tokens.surface, 0.5f),
                                        tokens.surfaceHover, tokens.surfaceActive).radius(6.0f)
                                .onClick([orgId = project->orgId, id] { openProjectWorkspace(orgId, id); }).build();
                            ui.text(tabId + ".name").position(8.0f, 0)
                                .size(std::max(0.0f, tabWidth - 30.0f), kInputHeight)
                                .text(project->name + " · " + orgName(project->orgId)).fontSize(kFontLabel)
                                .lineHeight(kInputHeight).color(active ? theme.titleText : theme.metaText)
                                .verticalAlign(core::VerticalAlign::Center).build();
                            components::button(ui, tabId + ".close").position(tabWidth - 22.0f, 3.0f)
                                .size(18.0f, 18.0f).icon(0xF00D).text("").iconSize(8.0f)
                                .theme(tokens, false).radius(9.0f).onClick([id] { closeProjectWorkspace(id); }).build();
                        }).build();
                        tx += tabW;
                    }
                }).build();
        }).build();

    const float settingsX = x + w - settingsW;
    drawIconBtn(ui, "workspace.settings", settingsX, y + 2.0f, 0xF013,
                theme, [] { g_page = Page::GlobalSettings; });
}

// 项目内部请求标签条。
export void drawRequestTabStrip(eui::Ui& ui, float x, float y, float w,
                                const AppTheme& theme) {
    const auto& tokens = theme.components;
    // 请求标签、加号和环境控件属于同一条紧凑工具岛；内部 viewport
    // 继续独立裁剪，避免 surface 影响横向滚动和 dropdown 命中。
    // 岛屿精确覆盖 26px 高的内容行，不再 y-2/28 外扩。
    drawIslandPanel(ui, "reqtabs.island", x, y, w, kInputHeight,
                    theme, theme.dark ? 0.56f : 0.76f);
    const float gearW = 26.0f;
    const float addW = 26.0f;
    // 环境下拉优先 170 宽；标签 viewport 至少保留 60，剩余不足时收缩 env
    // 而不是把 envX 推到容器左侧外面。
    const float envW = std::clamp(w - addW - gearW - kGap * 3.0f - 60.0f, 0.0f, 170.0f);
    const float envControlsW = envW + kGap + gearW;
    const float avail = nonNegative(w - envControlsW - kGap - addW);
    const int count = static_cast<int>(g_tabs.size());
    const float tabW = count > 0 ? std::clamp(avail / count, 110.0f, 200.0f) : 200.0f;
    const float totalW = std::max(0.0f, tabW * static_cast<float>(count));
    auto& scroll = g_requestScroll;
    scroll.maxOffset = std::max(0.0f, totalW - avail);
    scroll.offset = std::clamp(scroll.offset, 0.0f, scroll.maxOffset);

    ui.stack("reqtabs.viewport")
        .position(x, y).size(avail, 24.0f).clip().zIndex(20)
        .content([&] {
            components::mouseArea(ui, "reqtabs.wheel")
                .size(avail, 24.0f).scrollStep(36.0f).maxScrollStep(4.0f)
                .onScroll([](const components::MouseScrollEvent& event) {
                    auto& state = g_requestScroll;
                    const float delta = std::abs(event.deltaX) > 0.01f ? event.deltaX : -event.deltaY;
                    state.offset = std::clamp(state.offset - delta * 36.0f, 0.0f, state.maxOffset);
                }).build();
            ui.stack("reqtabs.content")
                .size(totalW, 24.0f).translateX(-scroll.offset).transformedHitTest()
                .content([&] {
                    float tx = 0.0f;
                    for (const auto& tab : g_tabs) {
                        const std::string tabId = "reqtabs.tab." + std::to_string(tab.uid);
                        const bool active = tab.uid == g_activeTabUid;
                        const float tabWidth = std::max(40.0f, tabW - 3.0f);
                        ui.stack(tabId).position(tx, 0).size(tabWidth, 24.0f).content([&] {
                            ui.rect(tabId + ".hit").size(tabWidth, 24.0f)
                                .states(active ? components::theme::withAlpha(tokens.primary, 0.20f)
                                               : components::theme::withAlpha(tokens.surface, 0.4f),
                                        tokens.surfaceHover, tokens.surfaceActive).radius(6.0f)
                                .onClick([uid = tab.uid] { g_activeTabUid = uid; persistSessionState(); }).build();
                            ui.text(tabId + ".method").position(6.0f, 0).size(34.0f, 24.0f)
                                .text(tabBadge(tab)).fontSize(8.0f).lineHeight(24.0f)
                                .color(tab.draft.kind == api::RequestKind::Http ? methodColor(tabBadge(tab), theme)
                                       : tab.draft.kind == api::RequestKind::WebSocket ? theme.redirect : theme.clientErr)
                                .verticalAlign(core::VerticalAlign::Center).build();
                            ui.text(tabId + ".name").position(42.0f, 0)
                                .size(std::max(0.0f, tabWidth - 66.0f), 24.0f)
                                .text(tabTitle(tab) + (tab.requestId == 0 ? " •" : ""))
                                .fontSize(kFontLabel).lineHeight(24.0f)
                                .color(active ? theme.titleText : theme.metaText)
                                .verticalAlign(core::VerticalAlign::Center).build();
                            components::button(ui, tabId + ".close").position(tabWidth - 22.0f, 3.0f)
                                .size(18.0f, 18.0f).icon(0xF00D).text("").iconSize(8.0f)
                                .theme(tokens, false).radius(9.0f).onClick([uid = tab.uid] {
                                    g_websocket.release(uid);
                                    g_tcp.release(uid);
                                    closeTab(uid);
                                }).build();
                        }).build();
                        tx += tabW;
                    }
                }).build();
        }).build();
    const float envX = std::max(x, x + w - envControlsW);
    drawIslandPanel(ui, "reqtabs.env.island", envX, y,
                    envControlsW, kInputHeight, theme,
                    theme.dark ? 0.50f : 0.72f);
    drawIconBtn(ui, "reqtabs.add", std::max(x, envX - addW - kGap), y + 1.0f,
                0xF067, theme, [] { (void)newDraftTab(); });

    const auto& envs = g_requests.environments();
    std::vector<std::string> envNames;
    int envSelected = 0;
    for (int i = 0; i < static_cast<int>(envs.size()); ++i) {
        envNames.push_back(envs[i].name);
        if (envs[i].id == g_requests.currentEnvId()) envSelected = i + 1;
    }
    envNames.insert(envNames.begin(), "未选择环境");
    // 极窄条里 env 下拉收缩到不可用时只留管理齿轮，不画零宽控件。
    if (envW >= 48.0f) {
        ui.stack("reqtabs.env.wrap")
            .position(envX, y)
            .size(envW, kInputHeight)
            .zIndex(30)
            .content([&] {
                registerSelectionPopup("reqtabs.env", g_topbarEnvOpen,
                                        [] { g_topbarEnvOpen = false; });
                components::dropdown(ui, "reqtabs.env")
                    .size(envW, kInputHeight)
                    .items(envNames)
                    .selected(envSelected)
                    .open(g_topbarEnvOpen)
                    .theme(tokens)
                    .onOpenChange([](bool open) {
                        g_topbarEnvOpen = open;
                        setSelectionPopupOpen("reqtabs.env", open);
                    })
                    .onChange([envs](int index) {
                        (void)g_requests.selectEnv(index > 0 ? envs[index - 1].id : 0);
                        persistSessionState();
                    })
                    .build();
            })
            .build();
    }
    drawIconBtn(ui, "reqtabs.env.manage", envX + envW + kGap, y + 1.0f,
                0xF0C9, theme, [] { g_envManageOpen = true; });
}
