// store/ui.cppm — 视图 store：页面 / 请求标签页 / 编辑器草稿 / 响应展示 /
// 压测视图状态 / 状态消息 / 确认弹窗。是「这个 UI 实现」的视图状态，但本身
// 不 import eui —— 纯数据 + 纯函数，立即模式 UI 每帧读它重绘。
// 线程纪律：只被 UI 线程读写（引擎结果经 poll* 在 UI 线程取回后写入）。
export module apitab.store.ui;

import std;
import apitab.api_engine;
import apitab.db;
import apitab.i18n;
import apitab.utils;

// ---- 页面 ----

export enum class Page { Home, GlobalSettings, Request, RequestEmpty, Load, History, ProjectSettings };
export Page g_page = Page::Home;  // 默认打开主页面

export enum class HomeTab { Projects, Members, Settings };
export HomeTab g_homeTab = HomeTab::Projects;

export bool isOverlayPage(Page page) {
    return page == Page::Home || page == Page::GlobalSettings;
}

export bool isProjectPage(Page page) {
    return page == Page::Request || page == Page::RequestEmpty || page == Page::Load || page == Page::ProjectSettings;
}

// ---- HTTP 方法 ----

export const std::vector<std::string> kMethods =
    {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};

// 全局首页当前选中的组织 id（仅视图状态，不直接驱动领域 store 的 currentOrgId）。
export std::int64_t g_homeSelectedOrgId = 0;

// ---- 编辑器草稿 ----

export enum class EditorTab { Params, Headers, Body, Cookies, Settings };

// 请求工作模式：调试保留完整编辑器，其余模式提供独立的设计/预览/用例/Mock 内容区。
export enum class RequestMode { Debug, Design, Preview, Cases, Mock };

export struct Draft {
    api::RequestKind kind = api::RequestKind::Http;
    std::string name = "未命名请求";
    int methodIndex = 0;             // kMethods 下标
    std::int64_t groupId = 0;        // 0 = 未分组；>0 = 所属分组 id
    std::string url;
    std::vector<api::KeyValue> params;
    std::vector<api::KeyValue> headers;
    std::vector<api::KeyValue> cookies;
    api::BodyKind bodyKind = api::BodyKind::None;
    std::string body;
    std::array<api::BodyContent, 7> bodyContents{};
    EditorTab tab = EditorTab::Params;
    RequestMode mode = RequestMode::Debug;
    bool followRedirects = true;
    bool allowJsonComments = true;
    std::string wsProtocol;
    int tcpConnectTimeoutSec = 15;
};

// 把集合项载入草稿。
export void fillDraft(Draft& draft, const db::SavedRequest& r) {
    draft.kind = r.kind;
    draft.name = r.name;
    draft.methodIndex = 0;
    for (int i = 0; i < static_cast<int>(kMethods.size()); ++i) {
        if (kMethods[i] == r.method) {
            draft.methodIndex = i;
            break;
        }
    }
    draft.groupId = r.groupId;
    draft.url = r.url;
    draft.params = r.params;
    draft.headers = r.headers;
    draft.cookies = r.cookies;
    draft.bodyKind = r.bodyKind;
    draft.body = r.body;
    draft.bodyContents = r.bodyContents;
    draft.followRedirects = r.followRedirects;
    draft.allowJsonComments = r.allowJsonComments;
    draft.wsProtocol = r.wsProtocol;
}

// 草稿 → 请求规格。finalUrl = 环境baseUrl + Path分组前缀 + 相对路径，
// 由领域 store 的 composeUrl 统一拼接（传入与 view store 当前环境一致的 envId）。
export api::RequestSpec buildSpec(const Draft& draft, const std::string& finalUrl) {
    api::RequestSpec spec;
    spec.method = kMethods[draft.methodIndex];
    spec.url = trim(finalUrl);
    spec.params = draft.params;
    spec.headers = draft.headers;
    spec.cookies = draft.cookies;
    spec.bodyKind = draft.bodyKind;
    const auto bodyIndex = static_cast<std::size_t>(draft.bodyKind);
    if (bodyIndex < draft.bodyContents.size()) {
        const auto& content = draft.bodyContents[bodyIndex];
        spec.body = content.text;
        spec.bodyFields = content.fields;
    } else {
        spec.body = draft.body;
    }
    spec.followRedirects = draft.followRedirects;
    spec.allowJsonComments = draft.allowJsonComments;
    return spec;
}

// ---- 请求标签页 ----
// 每个 tab 持有独立草稿与响应（切 tab 不丢编辑）；uid 会话内唯一且稳定
// （requestId 不可用作标识：多个未保存草稿都是 0）。

export struct RequestTab {
    std::int64_t uid = 0;
    std::int64_t requestId = 0;      // 关联的集合项；0 = 未保存的新草稿
    Draft draft;
    api::ResponseView response;
    bool hasResponse = false;
    float bodyScroll = 0.0f;
    api::WebSocketState wsState = api::WebSocketState::Disconnected;
    std::vector<api::WebSocketEvent> wsEvents;
    std::string wsMessage;
    bool wsBinary = false;
    float wsScroll = 0.0f;
    api::TcpState tcpState = api::TcpState::Disconnected;
    std::vector<api::TcpEvent> tcpEvents;
    std::string tcpMessage;
    api::TcpPayloadFormat tcpSendFormat = api::TcpPayloadFormat::Text;
    api::TcpPayloadFormat tcpReceiveFormat = api::TcpPayloadFormat::Text;
    float tcpScroll = 0.0f;
};

export std::vector<RequestTab> g_tabs;
export std::int64_t g_activeTabUid = 0;
export std::int64_t g_sendingTabUid = 0;   // 在途请求所属 tab（结果写回它）

std::int64_t g_nextTabUid = 1;

export RequestTab* findTab(std::int64_t uid) {
    for (auto& t : g_tabs) {
        if (t.uid == uid) return &t;
    }
    return nullptr;
}

// 当前 tab（保证存在：tab 列表为空时先开草稿 tab）。
export RequestTab& activeTab() {
    if (RequestTab* t = findTab(g_activeTabUid)) return *t;
    if (!g_tabs.empty()) {
        g_activeTabUid = g_tabs.front().uid;
        return g_tabs.front();
    }
    g_tabs.push_back(RequestTab{.uid = g_nextTabUid++});
    g_activeTabUid = g_tabs.back().uid;
    return g_tabs.back();
}

export Draft& activeDraft() { return activeTab().draft; }

// 侧栏点击：已有该请求的 tab → 切换；否则新开。
export RequestTab& openTab(const db::SavedRequest& r) {
    for (auto& t : g_tabs) {
        if (t.requestId == r.id) {
            g_activeTabUid = t.uid;
            return t;
        }
    }
    RequestTab tab{.uid = g_nextTabUid++, .requestId = r.id};
    fillDraft(tab.draft, r);
    g_tabs.push_back(std::move(tab));
    g_activeTabUid = g_tabs.back().uid;
    return g_tabs.back();
}

// 新建草稿 tab；不同协议共享同一标签条但由各自页面渲染。
export RequestTab& newDraftTab(api::RequestKind kind = api::RequestKind::Http,
                                std::int64_t groupId = 0) {
    RequestTab tab{.uid = g_nextTabUid++};
    tab.draft.kind = kind;
    tab.draft.groupId = groupId;
    switch (kind) {
        case api::RequestKind::Http: tab.draft.name = "未命名请求"; break;
        case api::RequestKind::WebSocket: tab.draft.name = "未命名 WebSocket"; break;
        case api::RequestKind::Tcp: tab.draft.name = "未命名 TCP"; break;
    }
    g_tabs.push_back(std::move(tab));
    g_activeTabUid = g_tabs.back().uid;
    return g_tabs.back();
}

export std::string tabBadge(const RequestTab& tab) {
    switch (tab.draft.kind) {
        case api::RequestKind::Http: return kMethods[std::clamp(tab.draft.methodIndex, 0, static_cast<int>(kMethods.size()) - 1)];
        case api::RequestKind::WebSocket: return "WS";
        case api::RequestKind::Tcp: return "TCP";
    }
    return "?";
}

export std::string tabTitle(const RequestTab& tab) { return tab.draft.name; }

// 关闭 tab；允许列表为空，空列表由请求空白页承载。
export void closeTab(std::int64_t uid) {
    std::erase_if(g_tabs, [uid](const RequestTab& t) { return t.uid == uid; });
    if (g_tabs.empty()) {
        g_activeTabUid = 0;
        g_page = Page::RequestEmpty;
        return;
    }
    if (g_activeTabUid == uid) g_activeTabUid = g_tabs.front().uid;
}

// ---- 标签页按项目暂存 / 恢复 ----
// 切项目不丢各项目打开的 tab（含未保存草稿）：切出时 stash，切入时恢复。

struct TabStash {
    std::vector<RequestTab> tabs;
    std::int64_t activeUid = 0;
};
std::unordered_map<std::int64_t, TabStash> g_tabStash;

export void stashTabs(std::int64_t projectId) {
    if (projectId == 0) return;
    g_tabStash[projectId] = TabStash{std::move(g_tabs), g_activeTabUid};
    g_tabs.clear();
}

export void restoreTabs(std::int64_t projectId) {
    if (const auto it = g_tabStash.find(projectId); it != g_tabStash.end()) {
        g_tabs = std::move(it->second.tabs);
        g_activeTabUid = it->second.activeUid;
        g_tabStash.erase(it);
    }
    // 空项目工作区保持无标签，由请求空白页显示创建入口。
}

// 丢弃某项目的暂存（项目被删除时调用）。
export void discardStash(std::int64_t projectId) { g_tabStash.erase(projectId); }

// 清空当前 tab（当前项目被删除时调用；自动补一个草稿 tab）。
export void clearTabs() {
    g_tabs.clear();
    g_activeTabUid = 0;
}

export std::string serializeIdList(const std::vector<std::int64_t>& ids) {
    std::string out;
    for (const std::int64_t id : ids) {
        if (!out.empty()) out += ',';
        out += std::to_string(id);
    }
    return out;
}

export std::vector<std::int64_t> parseIdList(std::string_view text) {
    std::vector<std::int64_t> ids;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(',', start);
        const std::string_view part = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        try { if (!part.empty()) ids.push_back(std::stoll(std::string(part))); } catch (...) {}
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return ids;
}
// 打开的项目是会话状态，关闭标签不会删除持久化项目。
export std::vector<std::int64_t> g_openProjectIds;
export std::int64_t g_activeProjectTabId = 0;

// ---- 响应展示 ----

export enum class ResponseTab { Body, Headers };
export ResponseTab g_responseTab = ResponseTab::Body;

// ---- 压测视图 ----

export std::string g_vusText = "10";
export std::string g_durationText = "30s";
export std::vector<std::string> g_loadOutput;   // 压测实时输出（封顶保留尾部）
export api::LoadSummary g_loadSummary;
export bool g_hasLoadSummary = false;
export bool g_showLoadRecords = false;          // 结果区：本次汇总 / 历史记录 切换

export void appendLoadOutput(std::vector<std::string> lines) {
    constexpr std::size_t kCap = 2000;
    for (auto& l : lines) g_loadOutput.push_back(std::move(l));
    if (g_loadOutput.size() > kCap) {
        g_loadOutput.erase(g_loadOutput.begin(),
                           g_loadOutput.end() - static_cast<std::ptrdiff_t>(kCap));
    }
}

export void restoreSessionState() {
    const std::string page = sessionPreference("page");
    if (!page.empty()) {
        try {
            const int value = std::stoi(page);
            if (value >= 0 && value <= static_cast<int>(Page::ProjectSettings)) g_page = static_cast<Page>(value);
        } catch (...) {}
    }
    const std::string org = sessionPreference("home_org");
    if (!org.empty()) try { g_homeSelectedOrgId = std::stoll(org); } catch (...) {}
    const std::string homeTab = sessionPreference("home_tab");
    if (!homeTab.empty()) try {
        const int value = std::stoi(homeTab);
        if (value >= 0 && value <= 2) g_homeTab = static_cast<HomeTab>(value);
    } catch (...) {}
}

export void persistSessionState() {
    saveSessionPreference("page", std::to_string(static_cast<int>(g_page)));
    saveSessionPreference("home_org", std::to_string(g_homeSelectedOrgId));
    saveSessionPreference("home_tab", std::to_string(static_cast<int>(g_homeTab)));
    saveSessionPreference("active_project", std::to_string(g_activeProjectTabId));
    saveSessionPreference("open_projects", serializeIdList(g_openProjectIds));
    if (const RequestTab* tab = findTab(g_activeTabUid)) {
        saveSessionPreference("active_request", std::to_string(tab->requestId));
        saveSessionPreference("editor_tab", std::to_string(static_cast<int>(tab->draft.tab)));
    } else {
        saveSessionPreference("active_request", "0");
        saveSessionPreference("editor_tab", "0");
    }
    saveSessionPreference("response_tab", std::to_string(static_cast<int>(g_responseTab)));
}

// ---- 组织 / 项目重命名内联编辑状态 ----

export std::int64_t g_renamingProjectId = 0;  // 0 = 无；否则正被重命名的项目 id
export std::string g_projectRenameText;
export bool g_renamingOrg = false;
export std::string g_orgRenameText;

// ---- 分组管理内联编辑状态 ----

export std::int64_t g_renamingGroupId = 0;   // 0 = 无；否则正被重命名的分组 id
export std::string g_groupRenameText;
export bool g_newGroupOpen = false;          // 新建分组弹窗开关
export std::string g_newGroupText;
export int g_newGroupMode = 0;               // 0 = 仅名称, 1 = 路径
export std::int64_t g_newGroupParentId = 0;  // 0 = 根目录
// 分组展开/收起（true = 收起）。默认全部展开。
export std::unordered_map<std::int64_t, bool> g_groupCollapsed;

// ---- 分组与请求类型菜单 ----
export bool g_groupMenuOpen = false;
export float g_groupMenuX = 0.0f;
export float g_groupMenuY = 0.0f;
export std::int64_t g_groupMenuTargetId = 0;
export bool g_requestTypeMenuOpen = false;
export float g_requestTypeMenuX = 0.0f;
export float g_requestTypeMenuY = 0.0f;
export std::int64_t g_requestTypeGroupId = 0;

// ---- 请求集合右键菜单 ----
export bool g_collectionMenuOpen = false;
export float g_collectionMenuX = 0.0f;
export float g_collectionMenuY = 0.0f;

// ---- 请求保存 / 请求行菜单 ----
export bool g_saveRequestNameOpen = false;
export std::string g_saveRequestNameText;
export bool g_requestMenuOpen = false;
export float g_requestMenuX = 0.0f;
export float g_requestMenuY = 0.0f;
export std::int64_t g_requestMenuTargetId = 0;
export bool g_renameRequestOpen = false;
export std::int64_t g_renameRequestId = 0;
export std::string g_requestRenameText;

export bool g_automationMenuOpen = false;
export float g_automationMenuX = 0.0f;
export float g_automationMenuY = 0.0f;
export std::int64_t g_automationMenuTargetId = 0;

export bool g_renameAutomationOpen = false;
export std::int64_t g_renameAutomationId = 0;
export std::int64_t g_automationRenameId = 0;
export std::string g_automationRenameText;

// ---- 环境管理弹窗 ----

export bool g_envManageOpen = false;
export std::int64_t g_envManageSelectedId = 0;
export struct EnvironmentVariableDraft {
    std::int64_t id = 0;
    std::string key;
    std::string value;
};
// 环境变量草稿按环境 id 隔离（切换环境/项目不串）。
export std::unordered_map<std::int64_t, std::vector<EnvironmentVariableDraft>> g_envVariableDrafts;
export std::int64_t g_nextEnvVariableDraftId = -1;
export bool g_globalCookieOpen = false;

// ---- 确认弹窗（删除项目/组织等破坏性操作）----

export struct ConfirmState {
    bool open = false;
    std::string title;
    std::string message;
    std::function<void()> action;
};
export ConfirmState g_confirm;

export void askConfirm(std::string title, std::string message, std::function<void()> action) {
    g_confirm.open = true;
    g_confirm.title = std::move(title);
    g_confirm.message = std::move(message);
    g_confirm.action = std::move(action);
}

// ---- 状态消息条 ----

export std::string g_statusMessage;

// showStatus 只能 UI 线程调用（g_statusMessage 非线程安全）。
export void showStatus(std::string message) { g_statusMessage = std::move(message); }

// 后台线程 → UI 线程的状态消息信箱（引擎/store 的异步错误经此中转）。
struct StatusMailbox {
    std::mutex m;
    std::vector<std::string> pending;
};
StatusMailbox g_statusMailbox;

export void postStatus(std::string message) {
    std::lock_guard lock(g_statusMailbox.m);
    g_statusMailbox.pending.push_back(std::move(message));
}

// UI 线程 compose 里调用：取出所有排队消息。
export std::vector<std::string> drainStatus() {
    std::lock_guard lock(g_statusMailbox.m);
    return std::exchange(g_statusMailbox.pending, {});
}
