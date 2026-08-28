// ui/request_page.cppm — 请求页：URL 编辑行 + Params/Headers/Body 编辑器 + 响应查看。
// 编辑对象是「当前请求标签页」的草稿（activeDraft），响应也按 tab 持有。
module;

#include "eui_ui.h"

export module apitab.ui.request_page;

import std;
import nlohmann.json;
import apitab.api_engine;
import apitab.db;
import apitab.store.requests;  // g_requests
import apitab.store.ui;        // activeTab / activeDraft / g_responseTab / ...
import apitab.ui.theme;
import apitab.ui.topbars;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

// 编辑器区滚动（Params/Headers 行溢出时）。
float g_editorScroll = 0.0f;
// method dropdown 受控 open 状态（模块私有）。
bool g_methodOpen = false;
// 环境 dropdown 受控 open 状态（模块私有）。
bool g_envOpen = false;
// 环境弹窗使用本地编辑缓冲，点击“保存”时才集中落库。
std::unordered_map<std::int64_t, std::string> g_envNameDrafts;
std::unordered_map<std::int64_t, std::string> g_envUrlDrafts;
std::int64_t g_envDraftProjectId = 0;
std::vector<db::GlobalCookie> g_globalCookieDrafts;
std::int64_t g_globalCookieDraftProjectId = 0;
std::int64_t g_nextTempGlobalCookieId = -1;

int bodyKindIndex(api::BodyKind kind) {
    switch (kind) {
        case api::BodyKind::None: return 0;
        case api::BodyKind::Json: return 1;
        case api::BodyKind::Text: return 2;
        case api::BodyKind::FormUrlEncoded: return 3;
        case api::BodyKind::FormData: return 4;
        case api::BodyKind::Xml: return 5;
        case api::BodyKind::GraphQL: return 6;
    }
    return 0;
}

api::BodyKind bodyKindFromIndex(int index) {
    switch (std::clamp(index, 0, 6)) {
        case 1: return api::BodyKind::Json;
        case 2: return api::BodyKind::Text;
        case 3: return api::BodyKind::FormUrlEncoded;
        case 4: return api::BodyKind::FormData;
        case 5: return api::BodyKind::Xml;
        case 6: return api::BodyKind::GraphQL;
        default: return api::BodyKind::None;
    }
}

void formatJsonBody() {
    Draft& draft = activeDraft();
    const auto index = static_cast<std::size_t>(draft.bodyKind);
    std::string& body = index < draft.bodyContents.size()
        ? draft.bodyContents[index].text : draft.body;
    try {
        const auto value = nlohmann::json::parse(body);
        body = value.dump(2);
        showStatus("JSON 已格式化");
    } catch (const std::exception& e) {
        std::string message = e.what();
        constexpr std::size_t maxDetail = 120;
        if (message.size() > maxDetail) message.resize(maxDetail);
        showStatus("JSON 格式错误: " + message);
    }
}

std::string composeFinalUrl(const Draft& draft) {
    return g_requests.composeUrl(draft.url, draft.groupId, g_requests.currentEnvId());
}

void sendCurrentRequest() {
    RequestTab& tab = activeTab();
    if (tab.draft.kind != api::RequestKind::Http) {
        showStatus("当前标签不是 HTTP 请求");
        return;
    }
    const std::string finalUrl = composeFinalUrl(tab.draft);
    if (finalUrl.empty()) {
        showStatus("URL 不能为空");
        return;
    }
    api::RequestSpec spec = buildSpec(tab.draft, finalUrl);
    if (!hasUriScheme(spec.url)) {
        spec.url = "http://" + spec.url;  // 裸域名补 http://（curl 无 scheme 会报错）
    }
    g_sendingTabUid = tab.uid;  // 结果写回发起 tab
    g_requests.send(spec, tab.requestId);
    showStatus("发送中…");
}

void persistCurrentRequest(const std::string& name) {
    RequestTab& tab = activeTab();
    db::SavedRequest r{
        .id = tab.requestId,
        .groupId = tab.draft.groupId,
        .name = name,
        .kind = tab.draft.kind,
        .wsProtocol = tab.draft.wsProtocol,
        .method = kMethods[tab.draft.methodIndex],
        .url = tab.draft.url,
        .params = tab.draft.params,
        .headers = tab.draft.headers,
        .cookies = tab.draft.cookies,
        .bodyKind = tab.draft.bodyKind,
        .body = tab.draft.body,
        .bodyContents = tab.draft.bodyContents,
        .followRedirects = tab.draft.followRedirects,
        .allowJsonComments = tab.draft.allowJsonComments,
    };
    const std::string err = g_requests.save(r);
    if (err.empty()) {
        tab.requestId = r.id;  // 草稿 tab 转正（去掉 • 标记）
        tab.draft.name = r.name;
        showStatus("已保存: " + r.name);
    } else {
        showStatus("保存失败: " + err);
    }
}

void saveCurrentRequest() {
    RequestTab& tab = activeTab();
    if (tab.requestId == 0) {
        g_saveRequestNameText = tab.draft.name == "未命名请求" ? "" : tab.draft.name;
        g_saveRequestNameOpen = true;
        return;
    }
    persistCurrentRequest(tab.draft.name);
}

void drawSaveRequestNameDialog(eui::Ui& ui, const eui::Screen& screen,
                               const AppTheme& theme) {
    if (!g_saveRequestNameOpen) return;
    drawInputDialog(ui, screen, theme, "req.save.name.dialog", "保存请求",
                    g_saveRequestNameText, "请求名称", "保存",
                    [] {
                        g_saveRequestNameOpen = false;
                        g_saveRequestNameText.clear();
                    },
                    [] {
                        const std::string name = trim(g_saveRequestNameText);
                        if (name.empty()) {
                            showStatus("请求名称不能为空");
                            return;
                        }
                        persistCurrentRequest(name);
                        if (activeTab().requestId != 0) {
                            g_saveRequestNameOpen = false;
                            g_saveRequestNameText.clear();
                        }
                    });
}

// ---- 编辑器区（按 tab 分发）----

void drawGlobalCookieDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    if (!g_globalCookieOpen) return;
    const auto& tokens = theme.components;
    const std::int64_t projectId = g_requests.currentProjectId();
    if (projectId == 0) { g_globalCookieOpen = false; return; }
    if (g_globalCookieDraftProjectId != projectId) {
        g_globalCookieDrafts = g_requests.globalCookies();
        g_globalCookieDraftProjectId = projectId;
    }
    auto& cookies = g_globalCookieDrafts;
    // 对话框尺寸 clamp 到屏幕；列表区是可滚动 viewport，footer 固定底部。
    const float dlgW = dialogWidth(screen.width, 480.0f);
    const float dlgH = dialogHeight(screen.height, 300.0f);
    const float pad = 16.0f;
    const float btnW = 74.0f;
    const float btnH = 24.0f;
    const float footerY = nonNegative(dlgH - btnH - 16.0f);
    const float addY = nonNegative(footerY - 28.0f);
    const float listY = 50.0f;
    const float listH = nonNegative(addY - listY - 4.0f);
    components::dialog(ui, "global.cookies.dialog")
        .open(true).screen(screen.width, screen.height).size(dlgW, dlgH)
        .title("全局 Cookies").theme(tokens)
        .content([&] {
            if (listH > 0.0f) {
                components::scrollView(ui, "global.cookies.scroll")
                    .position(pad, listY).size(nonNegative(dlgW - pad * 2.0f), listH)
                    .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                    .theme(tokens)
                    .content([&](eui::Ui& cu, float contentWidth, float) {
                        const float rowW = nonNegative(contentWidth);
                        const float nameW = std::min(120.0f, nonNegative(rowW - 32.0f) * 0.34f);
                        const float delW = 20.0f;
                        const float valueW = nonNegative(rowW - nameW - delW - 12.0f);
                        for (auto& cookie : cookies) {
                            const std::string id = "global.cookie." + std::to_string(cookie.id);
                            cu.stack(id).size(rowW, 28.0f).content([&] {
                                components::input(cu, id + ".name").position(0, 0)
                                    .size(nameW, 24.0f)
                                    .value(cookie.name).placeholder("名称").theme(tokens)
                                    .onChange([&cookie](const std::string& v) { cookie.name = v; }).build();
                                components::input(cu, id + ".value").position(nameW + 6.0f, 0)
                                    .size(valueW, 24.0f)
                                    .value(cookie.value).placeholder("值").theme(tokens)
                                    .onChange([&cookie](const std::string& v) { cookie.value = v; }).build();
                                components::button(cu, id + ".delete")
                                    .position(nonNegative(rowW - delW), 2.0f).size(delW, delW)
                                    .icon(0xF1F8).text("").iconSize(8.0f).theme(tokens, false)
                                    .radius(kButtonRadius)
                                    .onClick([&cookies, id = cookie.id] {
                                        std::erase_if(cookies, [id](const db::GlobalCookie& c) { return c.id == id; });
                                        if (id > 0) (void)g_requests.deleteGlobalCookie(id);
                                    }).build();
                            }).build();
                        }
                    })
                    .build();
            }
            components::button(ui, "global.cookies.add").position(pad, addY).size(80.0f, 24.0f)
                .icon(0xF067).text("添加").fontSize(kFontLabel).theme(tokens, false)
                .radius(kButtonRadius)
                .onClick([&cookies] { cookies.push_back({g_nextTempGlobalCookieId--, g_requests.currentProjectId(), {}, {}, true}); }).build();
            const float saveX = nonNegative(dlgW - pad - btnW);
            const float closeX = nonNegative(saveX - 8.0f - btnW);
            components::button(ui, "global.cookies.close").position(closeX, footerY).size(btnW, btnH)
                .text("关闭").fontSize(kFontLabel).theme(tokens, false)
                .radius(kButtonRadius)
                .onClick([] { g_globalCookieOpen = false; }).build();
            components::button(ui, "global.cookies.save").position(saveX, footerY).size(btnW, btnH)
                .text("保存").fontSize(kFontLabel).theme(tokens, true)
                .radius(kButtonRadius)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                .onClick([&cookies] {
                    for (auto& cookie : cookies) {
                        if (!cookie.name.empty()) {
                            const std::string err = g_requests.saveGlobalCookie(cookie);
                            if (!err.empty()) { showStatus("保存全局 Cookie 失败: " + err); return; }
                        }
                    }
                    g_globalCookieOpen = false;
                }).build();
        }).build();
}

void drawEditor(eui::Ui& ui, float x, float y, float w, float h, const AppTheme& theme) {
    const auto& tokens = theme.components;
    Draft& draft = activeDraft();
    switch (draft.tab) {
        case EditorTab::Params: {
            components::scrollView(ui, "editor.params.scroll")
                .position(x, y).size(w, h).offset(g_editorScroll).theme(tokens)
                .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                .onChange([](float v) { g_editorScroll = v; })
                .content([&](eui::Ui& cu, float contentWidth, float) {
                    drawFieldTable(cu, "editor.params.table", 0, 0,
                                   nonNegative(contentWidth - 4.0f), draft.params, theme, true);
                })
                .build();
            break;
        }
        case EditorTab::Headers: {
            auto& items = draft.headers;
            components::scrollView(ui, "editor.headers.scroll")
                .position(x, y).size(w, h).offset(g_editorScroll).theme(tokens)
                .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                .onChange([](float v) { g_editorScroll = v; })
                .content([&](eui::Ui& cu, float contentWidth, float) {
                    drawFieldTable(cu, "editor.headers.table", 0, 0,
                                   nonNegative(contentWidth - 4.0f), items, theme, false);
                })
                .build();
            break;
        }
        case EditorTab::Cookies: {
            auto& items = draft.cookies;
            const float cookieListH = nonNegative(h - 32.0f);
            if (cookieListH > 0.0f) {
                components::scrollView(ui, "editor.cookies.scroll")
                    .position(x, y).size(w, cookieListH).theme(tokens)
                    .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                    .content([&](eui::Ui& cu, float contentWidth, float) {
                        drawFieldTable(cu, "editor.cookies.table", 0, 0,
                                           nonNegative(contentWidth - 4.0f), items, theme, false);
                    }).build();
            }
            break;
        }
        case EditorTab::Settings: {
            ui.text("editor.settings.title")
                .position(x, y).size(w, 24.0f).text("请求设置")
                .fontSize(kFontBody).color(theme.titleText).build();
            ui.stack("editor.settings.redirects.wrap")
                .position(x, y + 34.0f).size(w, 24.0f)
                .content([&] {
                    components::checkbox(ui, "editor.settings.redirects")
                        .size(w, 24.0f)
                        .text("自动跟随重定向").checked(draft.followRedirects)
                        .theme(tokens)
                        .style(checkboxStyle(theme))
                        .onChange([](bool value) { activeDraft().followRedirects = value; }).build();
                }).build();
            ui.stack("editor.settings.json.comments.wrap")
                .position(x, y + 66.0f).size(w, 24.0f)
                .content([&] {
                    components::checkbox(ui, "editor.settings.json.comments")
                        .size(w, 24.0f)
                        .text("兼容带注释的 JSON").checked(draft.allowJsonComments)
                        .theme(tokens)
                        .style(checkboxStyle(theme))
                        .onChange([](bool value) { activeDraft().allowJsonComments = value; }).build();
                }).build();
            break;
        }
        case EditorTab::Body: {
            constexpr float selectorH = 24.0f;
            const float selectorW = std::min(430.0f, w);
            ui.stack("editor.body.kind.wrap")
                .position(x, y).size(selectorW, selectorH)
                .content([&] {
                components::segmented(ui, "editor.body.kind")
                        .size(selectorW, selectorH)
                        .items({"None", "JSON", "Text", "x-www-form-urlencoded", "form-data", "XML", "GraphQL"})
                        .selected(bodyKindIndex(draft.bodyKind))
                        .fontSize(9.0f)
                        .theme(tokens).style(segmentedStyle(theme))
                        .onChange([](int i) {
                            activeDraft().bodyKind = bodyKindFromIndex(i);
                        })
                        .build();
                })
                .build();
            if (draft.bodyKind == api::BodyKind::None) {
                ui.text("editor.body.none")
                    .position(x, y + selectorH + 12.0f).size(w, 20.0f)
                    .text("None：此次请求不发送请求体")
                    .fontSize(kFontLabel).color(theme.hintText).build();
            } else {
                const float bodyToolbarH = draft.bodyKind == api::BodyKind::Json ? 26.0f : 0.0f;
                const float bodyH = nonNegative(h - selectorH - 6.0f - bodyToolbarH);
                if (bodyH <= 0.0f) break;
                const char* placeholder = draft.bodyKind == api::BodyKind::Json
                    ? "例如：{\"name\":\"apitab\"}"
                    : draft.bodyKind == api::BodyKind::FormUrlEncoded
                        ? "例如：name=apitab&mode=test"
                        : draft.bodyKind == api::BodyKind::FormData
                            ? "每行一个字段：name=apitab"
                            : draft.bodyKind == api::BodyKind::Xml
                                ? "例如：<user><name>apitab</name></user>"
                                : draft.bodyKind == api::BodyKind::GraphQL
                                    ? "输入 GraphQL 查询或 mutation"
                                    : "输入纯文本请求体，例如：hello apitab";
                const auto bodyIndex = static_cast<std::size_t>(draft.bodyKind);
                api::BodyContent* bodyContent = bodyIndex < draft.bodyContents.size()
                    ? &draft.bodyContents[bodyIndex] : nullptr;
                std::string& bodyText = bodyContent ? bodyContent->text : draft.body;
                const bool formBody = draft.bodyKind == api::BodyKind::FormUrlEncoded ||
                                      draft.bodyKind == api::BodyKind::FormData;
                if (formBody) {
                    auto& fields = bodyContent->fields;
                    components::scrollView(ui, "editor.body.form.scroll")
                        .position(x, y + selectorH + 6.0f).size(w, bodyH).theme(tokens)
                        .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                        .content([&](eui::Ui& cu, float contentWidth, float) {
                            drawFieldTable(cu, "editor.body.form.table", 0, 0,
                                           nonNegative(contentWidth - 4.0f), fields, theme, false);
                        }).build();
                    break;
                }
                if (draft.bodyKind == api::BodyKind::Json) {
                    components::button(ui, "editor.body.format")
                        .position(x + std::max(0.0f, w - 82.0f), y + selectorH + 6.0f)
                        .size(82.0f, 22.0f)
                        .icon(0xF1C9).text("格式化").fontSize(kFontLabel)
                        .iconSize(kCardActionIconSize).theme(tokens, false)
                        .radius(kButtonRadius)
                        .onClick([] { formatJsonBody(); })
                        .build();
                }
                components::input(ui, "editor.body")
                    .position(x, y + selectorH + 6.0f + bodyToolbarH)
                    .size(w, bodyH)
                    .value(bodyText).placeholder(placeholder).multiline()
                    .fontFamily("monospace")
                    .theme(tokens)
                    .onChange([](const std::string& v) {
                        Draft& current = activeDraft();
                        const auto index = static_cast<std::size_t>(current.bodyKind);
                        if (index < current.bodyContents.size()) current.bodyContents[index].text = v;
                        else current.body = v;
                    })
                    .build();
            }
            break;
        }
    }
}

// ---- 响应区 ----

void drawResponse(eui::Ui& ui, float x, float y, float w, float h, const AppTheme& theme) {
    const auto& tokens = theme.components;
    RequestTab& tab = activeTab();

    // 响应元信息行：状态码（着色）/ 耗时 / 大小
    if (tab.hasResponse) {
        const auto& resp = tab.response;
        const std::string statusText =
            resp.ok ? std::to_string(resp.status) : "ERR";
        ui.text("resp.status")
            .position(x, y)
            .size(60.0f, 22.0f)
            .text(statusText)
            .fontSize(kFontBody + 2.0f)
            .lineHeight(22.0f)
            .color(resp.ok ? statusColor(theme, resp.status) : theme.serverErr)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
        std::string meta;
        if (resp.ok) {
            meta = std::format("{}  ·  {}  ·  DNS {:.0f}ms / 连接 {:.0f}ms / TLS {:.0f}ms",
                               formatMs(resp.totalMs), formatBytes(resp.sizeBytes),
                               resp.dnsMs, resp.connectMs, resp.tlsMs);
        } else {
            meta = resp.error;
        }
        ui.text("resp.meta")
            .position(x + 66.0f, y)
            .size(std::max(0.0f, w - 66.0f - 170.0f), 22.0f)
            .text(meta)
            .fontSize(kFontLabel)
            .lineHeight(22.0f)
            .color(resp.ok ? theme.metaText : theme.serverErr)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    } else {
        ui.text("resp.hint")
            .position(x, y)
            .size(std::max(0.0f, w - 170.0f), 22.0f)
            .text(g_requests.busy() ? "等待响应…" : "响应")
            .fontSize(kFontBody)
            .lineHeight(22.0f)
            .color(theme.hintText)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }

    // Body / Headers 切换（右上）。segmented 无 position —— stack 包裹定位。
    ui.stack("resp.tabs.wrap")
        .position(x + std::max(0.0f, w - 150.0f), y)
        .size(150.0f, 22.0f)
        .content([&] {
            components::segmented(ui, "resp.tabs")
                .size(150.0f, 22.0f)
                .items({"Body", "Headers"})
                .selected(g_responseTab == ResponseTab::Body ? 0 : 1)
                .fontSize(kFontLabel)
                .theme(tokens)
                .style(segmentedStyle(theme))
                .onChange([](int i) {
                    g_responseTab = i == 0 ? ResponseTab::Body : ResponseTab::Headers;
                    persistSessionState();
                })
                .build();
        })
        .build();

    // 内容区
    const float cy = y + 28.0f;
    const float ch = nonNegative(h - 28.0f);
    if (!tab.hasResponse || ch <= 0.0f) return;

    // 内容区直接使用响应岛的内边距，避免再叠加一层 panel 背景。
    const float scrollW = nonNegative(w - kPanelPad * 2.0f);
    const float scrollH = nonNegative(ch - kPanelPad * 2.0f);
    if (scrollW <= 0.0f || scrollH <= 0.0f) return;

    components::scrollView(ui, "resp.scroll")
        .position(x + kPanelPad, cy + kPanelPad)
        .size(scrollW, scrollH)
        .offset(tab.bodyScroll)
        .theme(tokens)
        .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
        .onChange([](float v) { activeTab().bodyScroll = v; })
        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
            const float textW = nonNegative(contentWidth - 8.0f);
            if (g_responseTab == ResponseTab::Body) {
                // 换行后的真实高度进 scroll root，不用 viewport 高度冒充内容高。
                const std::string& body = tab.response.body;
                const float bodyH = std::max(viewportH,
                    measureWrappedTextHeight(body.empty() ? "(空响应体)" : body,
                                             textW, kFontMono, "monospace"));
                cu.text("resp.body")
                    .position(4.0f, 4.0f)
                    .size(textW, bodyH)
                    .text(body.empty() ? "(空响应体)" : body)
                    .fontFamily("monospace")
                    .fontSize(kFontMono)
                    .color(theme.bodyText)
                    .wrap(true)
                    .build();
            } else {
                int i = 0;
                for (const auto& hv : tab.response.headers) {
                    const std::string rowId = "resp.hdr." + std::to_string(i++);
                    // 行包 stack（scroll content 是 column，直接 .position 会被重排）。
                    // 长 header 值单行截断（不 wrap），行高恒定，scroll 测量精确。
                    cu.stack(rowId)
                        .size(nonNegative(contentWidth - 8.0f), 16.0f)
                        .content([&] {
                            const float keyW = std::max(0.0f, contentWidth * 0.34f);
                            cu.text(rowId + ".k")
                                .position(0, 0)
                                .size(keyW, 16.0f)
                                .text(hv.key)
                                .fontSize(kFontMono)
                                .color(theme.metaText)
                                .build();
                            cu.text(rowId + ".v")
                                .position(keyW + 10.0f, 0)
                                .size(std::max(0.0f, contentWidth - keyW - 14.0f - 8.0f), 16.0f)
                                .text(hv.value)
                                .fontSize(kFontMono)
                                .color(theme.bodyText)
                                .build();
                        })
                        .build();
                }
                if (tab.response.headers.empty()) {
                    cu.text("resp.hdr.empty")
                        .position(4.0f, 4.0f)
                        .size(textW, 16.0f)
                        .text("(无响应头)")
                        .fontSize(kFontLabel)
                        .color(theme.hintText)
                        .build();
                }
            }
        })
        .build();
}

} // namespace

export void drawRequestPage(eui::Ui& ui, float x, float y, float w, float h,
                            const AppTheme& theme) {
    const auto& tokens = theme.components;
    Draft& draft = activeDraft();

    // ---- 第 1 行：method + URL（弹性）+ 发送 + 保存 ----
    // 单行最小跨度 = 96+120+64+52+3*gap = 350；低于阈值换成两行：
    // 第一行 method + URL（占满），第二行 发送/保存。URL 不再用 min 120 硬顶。
    const float sendW = 64.0f, saveW = 52.0f, methodW = 96.0f;
    const bool toolbarTwoRows = w < 350.0f;
    const float toolbarH = toolbarTwoRows ? kInputHeight * 2.0f + kGap : kInputHeight;
    // 请求标签、请求目标、操作、编辑器合并为同一个工作流岛屿；标签条也在岛内
    // 作为第一行布局，避免页面层写死位置与内容坐标互相覆盖。
    const float requestIslandH = nonNegative(h * 0.54f);
    const float islandX = x;
    const float islandY = y;
    const float islandW = w;
    const float islandH = requestIslandH;
    const float innerX = kPanelPad;
    const float innerW = nonNegative(w - kPanelPad * 2.0f);
    const float tabsX = 4.0f;
    const float tabsY = 4.0f;
    const float tabsW = nonNegative(w - tabsX * 2.0f);
    const float tabsH = kInputHeight;
    const float toolbarY = tabsY + tabsH + kIslandGap;
    // Method 与 URL 是同一组目标输入，紧贴排列；发送/保存仍作为操作组保留语义间距。
    const float targetGap = 0.0f;
    const float actionGap = kGap;
    const float urlW = toolbarTwoRows
        ? nonNegative(innerW - methodW - targetGap)
        : nonNegative(innerW - methodW - sendW - saveW - actionGap * 2.0f - targetGap);
    drawIsland(ui, "req.request.island", islandX, islandY, islandW, islandH, theme,
               theme.dark ? 0.56f : 0.78f, kIslandPopupZIndex, [&] {
    // 标签栏与请求编辑内容共享同一个一级岛屿；在岛内保留一丝上下左右留白，
    // 让标签栏与岛屿边框不要贴合。
    drawRequestTabStrip(ui, tabsX, tabsY, tabsW, theme, false);
    const float toolbarX = innerX;
    const float targetW = methodW + targetGap + urlW;
    const float actionsY = toolbarTwoRows ? toolbarY + kInputHeight + kGap : toolbarY;
    const float actionsX = toolbarTwoRows ? toolbarX : toolbarX + targetW + actionGap;

    ui.stack("req.method.wrap")
        .position(toolbarX, toolbarY)
        .size(methodW, kInputHeight)
        .zIndex(kPopupZIndex)  // 弹层压过下方编辑器与响应岛屿
        .content([&] {
            registerSelectionPopup("req.method", g_methodOpen,
                                    [] { g_methodOpen = false; });
            components::dropdown(ui, "req.method")
                .size(methodW, kInputHeight)
                .items(kMethods)
                .selected(draft.methodIndex)
                .open(g_methodOpen)
                .theme(tokens)
                .onOpenChange([](bool o) {
                    g_methodOpen = o;
                    setSelectionPopupOpen("req.method", o);
                })
                .onChange([](int i) { activeDraft().methodIndex = i; })
                .build();
        })
        .build();

    const bool busy = g_requests.busy();
    const float urlX = toolbarX + methodW + targetGap;
    components::input(ui, "req.url")
        .position(urlX, toolbarY)
        .size(urlW, kInputHeight)
        .value(draft.url)
        .placeholder("相对路径，如 users / v1/users")
        .fontFamily("monospace")
        .theme(tokens)
        .onChange([](const std::string& v) { activeDraft().url = v; })
        .onEnter([] { sendCurrentRequest(); })
        .build();

    components::button(ui, "req.send")
        .position(actionsX, actionsY)
        .size(sendW, kButtonHeight)
        .icon(busy ? 0xF04D : 0xF1D8)  // 发送中显示停止块（点击=取消）
        .text(busy ? "取消" : "发送")
        .fontSize(kFontBody)
        .theme(tokens, true)
        .textColor(onPrimaryColor(theme))   // 白底主色 → 深色字/图标才可读
        .iconColor(onPrimaryColor(theme))
        .radius(kButtonRadius)
        .onClick([busy] {
            if (busy) {
                g_requests.cancel();
                showStatus("已取消");
            } else {
                sendCurrentRequest();
            }
        })
        .build();

    components::button(ui, "req.save")
        .position(actionsX + sendW + kGap, actionsY)
        .size(saveW, kButtonHeight)
        .icon(0xF0C7)  // fa-floppy-disk
        .text("保存")
        .fontSize(kFontBody)
        .theme(tokens, false)
        .radius(kButtonRadius)
        .onClick([] { saveCurrentRequest(); })
        .build();

    // ---- 第 1.5 行：最终 URL 预览（环境 / 路径分组前缀生效时显示完整目标）----
    const std::string finalUrl = composeFinalUrl(draft);
    if (finalUrl != draft.url) {
        ui.text("req.url.preview")
            .position(innerX, toolbarY + toolbarH + 2.0f)
            .size(innerW, 14.0f)
            .text("→ " + (finalUrl.empty() ? std::string("（输入相对路径）") : finalUrl))
            .fontSize(kFontLabel)
            .fontFamily("monospace")
            .lineHeight(14.0f)
            .color(theme.hintText)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }

    // ---- 工作模式 + 调试编辑器标签 ----
    const float modeY = toolbarY + toolbarH + 18.0f;
    const float modeWidth = std::min(430.0f, innerW);
    ui.stack("req.mode.wrap").position(innerX, modeY).size(modeWidth, 24.0f).zIndex(12).content([&] {
        components::segmented(ui, "req.mode")
            .size(modeWidth, 24.0f)
            .items({"调试", "设计", "预览", "用例", "Mock"})
            .selected(static_cast<int>(draft.mode)).fontSize(kFontLabel)
            .theme(tokens).style(segmentedStyle(theme))
            .onChange([](int i) { activeDraft().mode = static_cast<RequestMode>(std::clamp(i, 0, 4)); })
            .build();
    }).build();

    const float tabY = modeY + 30.0f;
    const float tabWidth = std::min(430.0f, innerW);
    if (draft.mode == RequestMode::Debug) {
        ui.stack("req.tabs.wrap").position(innerX, tabY).size(tabWidth, 24.0f).content([&] {
            components::segmented(ui, "req.tabs").size(tabWidth, 24.0f)
                .items({"Params", "Headers", "Body", "Cookies", "设置"})
                .selected(static_cast<int>(draft.tab)).fontSize(kFontLabel)
                .theme(tokens).style(segmentedStyle(theme))
                .onChange([](int i) { activeDraft().tab = static_cast<EditorTab>(std::clamp(i, 0, 4)); persistSessionState(); })
                .build();
        }).build();
        const float metaX = innerX + tabWidth + kGap;
        const float metaW = innerX + innerW - metaX;
        if (metaW > 60.0f) ui.text("req.tab.meta").position(metaX, tabY).size(metaW, 24.0f)
            .text(std::format("{} 个参数 · {} 个请求头 · {} 个 Cookie", draft.params.size(), draft.headers.size(), draft.cookies.size()))
            .fontSize(kFontLabel).lineHeight(24.0f).color(theme.hintText)
            .verticalAlign(core::VerticalAlign::Center).build();
    } else {
        const char* modeTitle = draft.mode == RequestMode::Design ? "设计模式" : draft.mode == RequestMode::Preview ? "预览模式" : draft.mode == RequestMode::Cases ? "用例模式" : "Mock 模式";
        ui.text("req.mode.title").position(innerX, tabY).size(innerW, 24.0f).text(modeTitle)
            .fontSize(kFontBody).lineHeight(24.0f).color(theme.titleText).build();
        ui.text("req.mode.content").position(innerX, tabY + 34.0f).size(innerW, 42.0f)
            .text(draft.mode == RequestMode::Design ? "在这里定义请求结构与字段说明。调试模式可直接发送请求。" : draft.mode == RequestMode::Preview ? "请求预览：" + composeFinalUrl(draft) : draft.mode == RequestMode::Cases ? "暂无用例。点击保存后可在此添加断言与场景。" : "Mock 响应尚未配置。可在此设置状态码、响应头与响应体。")
            .fontSize(kFontLabel).lineHeight(20.0f).color(theme.metaText).wrap(true).build();
    }

    // 请求编辑器与响应仍分层；非调试模式显示明确内容，不与编辑器重叠。
    const float editorY = draft.mode == RequestMode::Debug ? tabY + 30.0f : tabY + 86.0f;
    const float editorH = nonNegative(islandH - editorY - kPanelPad);
    drawHorizontalDivider(ui, "req.request.divider.editor", innerX, editorY - 6.0f, innerW, theme);
    if (editorH > 0.0f && draft.mode == RequestMode::Debug) drawEditor(ui, innerX, editorY, innerW, editorH, theme);
    });
    const float respY = y + requestIslandH + kIslandGap;
    const float respH = nonNegative(y + h - respY);
    if (respH > 0.0f) {
        drawIsland(ui, "req.response.island", x, respY, w, respH, theme,
                   theme.dark ? 0.64f : 0.84f, [&] {
            drawResponse(ui, kPanelPad, kPanelPad, innerW,
                         nonNegative(respH - kPanelPad), theme);
        });
    }
}

// 环境管理弹窗（需要全屏坐标系，由 app.cpp 传入 screen）。
// 列表内直接编辑名称 / baseUrl（即时落库），＋ 新建，🗑 删除（弹确认）。
export void drawRequestPageDialogs(eui::Ui& ui, const eui::Screen& screen,
                                   const AppTheme& theme) {
    drawSaveRequestNameDialog(ui, screen, theme);
    drawGlobalCookieDialog(ui, screen, theme);
    if (g_globalCookieOpen || !g_envManageOpen) return;
    const auto& tokens = theme.components;
    const auto& envs = g_requests.environments();
    if (g_envDraftProjectId != g_requests.currentProjectId()) {
        g_envNameDrafts.clear();
        g_envUrlDrafts.clear();
        g_envVariableDrafts.clear();
        g_envDraftProjectId = g_requests.currentProjectId();
    }
    for (const auto& e : envs) {
        g_envNameDrafts.try_emplace(e.id, e.name);
        g_envUrlDrafts.try_emplace(e.id, e.baseUrl);
    }
    if (g_envManageSelectedId == 0 || !g_requests.findEnvironment(g_envManageSelectedId))
        g_envManageSelectedId = envs.empty() ? 0 : envs.front().id;
    const db::Environment* selected = g_requests.findEnvironment(g_envManageSelectedId);
    constexpr float rowH = 28.0f;
    const float dlgW = dialogWidth(screen.width, 620.0f);
    const float dlgH = dialogHeight(screen.height, 420.0f);
    const float pad = 16.0f;
    const float footerY = nonNegative(dlgH - kButtonHeight - pad);
    const float bodyY = 48.0f;
    const float bodyH = nonNegative(footerY - bodyY - 10.0f);
    const float leftW = std::min(190.0f, std::max(128.0f, dlgW * 0.32f));
    const float rightX = leftW + pad * 2.0f;
    const float rightW = nonNegative(dlgW - rightX - pad);
    components::dialog(ui, "req.env.dialog").open(true).screen(screen.width, screen.height)
        .size(dlgW, dlgH).title("环境管理（当前项目）").theme(tokens).content([&] {
            ui.rect("req.env.list.panel").position(pad, bodyY).size(leftW, bodyH)
                .color(components::theme::withAlpha(tokens.surface, theme.dark ? .42f : .72f))
                .radius(kPanelRadius).build();
            components::scrollView(ui, "req.env.list.scroll").position(pad + 6.0f, bodyY + 8.0f)
                .size(nonNegative(leftW - 12.0f), nonNegative(bodyH - 16.0f))
                .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap).theme(tokens)
                .content([&](eui::Ui& cu, float cw, float) {
                    if (envs.empty()) cu.text("req.env.list.empty").position(4, 4).size(cw - 8, 30)
                        .text("暂无环境").fontSize(kFontLabel).color(theme.hintText).build();
                    float y = 0;
                    for (const auto& e : envs) {
                        const std::string id = "req.env.item." + std::to_string(e.id);
                        const bool active = e.id == g_envManageSelectedId;
                        cu.stack(id).position(0, y).size(cw, rowH).content([&] {
                            cu.rect(id + ".bg").fill().color(active
                                ? components::theme::withAlpha(tokens.primary, .2f)
                                : core::Color{0,0,0,0}).radius(5).build();
                            cu.text(id + ".text").position(8, 0).size(cw - 16, rowH)
                                .text(e.name).fontSize(kFontBody).lineHeight(rowH).color(theme.bodyText)
                                .verticalAlign(core::VerticalAlign::Center).build();
                            cu.rect(id + ".hit").fill().color(core::Color{0,0,0,0})
                                .onClick([id = e.id] { g_envManageSelectedId = id; }).build();
                        }).build();
                        y += rowH + 4.0f;
                    }
                }).build();
            ui.rect("req.env.detail.panel").position(rightX, bodyY).size(rightW, bodyH)
                .color(components::theme::withAlpha(tokens.surface, theme.dark ? .3f : .55f))
                .radius(kPanelRadius).build();
            if (selected) {
                const auto id = selected->id;
                const float fieldW = nonNegative(rightW - 24.0f);
                ui.text("req.env.detail.title").position(rightX + 12, bodyY + 12).size(fieldW, 22)
                    .text("环境详情").fontSize(kFontBody + 1).color(theme.titleText).build();
                components::input(ui, "req.env.detail.name").position(rightX + 12, bodyY + 44)
                    .size(fieldW, kInputHeight).value(g_envNameDrafts[id]).placeholder("环境名称")
                    .theme(tokens).onChange([id](const std::string& v) { g_envNameDrafts[id] = v; }).build();
                components::input(ui, "req.env.detail.url").position(rightX + 12, bodyY + 78)
                    .size(fieldW, kInputHeight).value(g_envUrlDrafts[id]).placeholder("基础 URL，例如 http://localhost:8080/")
                    .fontFamily("monospace").fontSize(kFontMono).theme(tokens)
                    .onChange([id](const std::string& v) { g_envUrlDrafts[id] = v; }).build();
                ui.text("req.env.vars.title").position(rightX + 12, bodyY + 116).size(fieldW, 22)
                    .text("环境变量").fontSize(kFontBody).color(theme.bodyText).build();
                ui.text("req.env.vars.hint").position(rightX + 12, bodyY + 138).size(fieldW, 30)
                    .text("当前版本保存于本地会话；数据库模型暂不支持环境变量持久化。")
                    .fontSize(kFontLabel).color(theme.hintText).wrap(true).build();
                const float varsY = bodyY + 174.0f;
                const float addY = nonNegative(bodyY + bodyH - 30.0f);
                const float varsH = nonNegative(addY - varsY - 6.0f);
                if (varsH > 0.0f) {
                    components::scrollView(ui, "req.env.vars.scroll")
                        .position(rightX + 12, varsY).size(fieldW, varsH).theme(tokens)
                        .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
                            const float keyW = std::max(40.0f, contentWidth * .34f);
                            const float valueW = std::max(40.0f, contentWidth - keyW - 6.0f);
                            auto& vars = g_envVariableDrafts[g_envManageSelectedId];
                            for (auto& var : vars) {
                                const auto vid = var.id;
                                const std::string base = "req.env.var." + std::to_string(vid);
                                cu.stack(base + ".row").size(contentWidth, 28.0f).content([&] {
                                    components::input(cu, base + ".key").position(0, 0).size(keyW, 24)
                                        .value(var.key).placeholder("键").theme(tokens)
                                        .onChange([vid, envId = g_envManageSelectedId](const std::string& v) { for (auto& x : g_envVariableDrafts[envId]) if (x.id == vid) x.key = v; }).build();
                                    components::input(cu, base + ".value").position(keyW + 6.0f, 0).size(valueW, 24)
                                        .value(var.value).placeholder("值").theme(tokens)
                                        .onChange([vid, envId = g_envManageSelectedId](const std::string& v) { for (auto& x : g_envVariableDrafts[envId]) if (x.id == vid) x.value = v; }).build();
                                }).build();
                            }
                            if (vars.empty()) cu.text("req.env.vars.empty").position(0, 0).size(contentWidth, 24)
                                .text("暂无变量").fontSize(kFontLabel).color(theme.hintText).build();
                        }).build();
                }
                components::button(ui, "req.env.vars.add").position(rightX + 12, addY).size(82, 24)
                    .icon(0xF067).text("添加").fontSize(kFontLabel).iconSize(8).theme(tokens, false)
                    .radius(kButtonRadius).onClick([] { g_envVariableDrafts[g_envManageSelectedId].push_back({g_nextEnvVariableDraftId--, {}, {}}); }).build();
                components::button(ui, "req.env.detail.delete").position(rightX + rightW - 78, bodyY + 12).size(66, 22)
                    .icon(0xF1F8).text("删除").fontSize(kFontLabel).iconSize(8).theme(tokens, false)
                    .radius(kButtonRadius).onClick([id, name = selected->name] { askConfirm("删除环境", std::format("将删除环境「{}」。", name), [id] {
                        const std::string err = g_requests.deleteEnvironment(id); if (err.empty()) { g_envManageSelectedId = 0; showStatus("环境已删除"); } else showStatus("删除失败: " + err);
                    }); }).build();
            }
            const float saveX = nonNegative(dlgW - pad - 74.0f);
            components::button(ui, "req.env.dialog.cancel").position(nonNegative(saveX - 82), footerY)
                .size(74, kButtonHeight).text("取消").fontSize(kFontBody).theme(tokens, false)
                .radius(kButtonRadius).onClick([] { g_envManageOpen = false; g_envManageSelectedId = 0; }).build();
            components::button(ui, "req.env.dialog.save").position(saveX, footerY)
                .size(74, kButtonHeight).text("保存").fontSize(kFontBody).theme(tokens, true)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme)).radius(kButtonRadius)
                .onClick([id = g_envManageSelectedId] {
                    if (id == 0) return; const std::string name = trim(g_envNameDrafts[id]);
                    if (name.empty()) { showStatus("环境名称不能为空"); return; }
                    const std::string err = g_requests.updateEnvironment(id, name, trim(g_envUrlDrafts[id]));
                    if (err.empty()) { g_envManageOpen = false; showStatus("环境已保存"); } else showStatus("保存环境失败: " + err);
                }).build();
            components::button(ui, "req.env.dialog.add").position(pad, footerY).size(92, kButtonHeight)
                .icon(0xF067).text("新建环境").fontSize(kFontBody).iconSize(8).theme(tokens, false)
                .radius(kButtonRadius).onClick([] { const std::string err = g_requests.createEnvironment("新环境", "http://localhost:8080/");
                    if (err.empty()) { if (!g_requests.environments().empty()) g_envManageSelectedId = g_requests.environments().back().id; showStatus("环境已创建"); } else showStatus("创建环境失败: " + err); }).build();
        }).build();
}
