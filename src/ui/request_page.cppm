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

std::size_t lineCount(std::string_view text) {
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

void drawLineNumbers(eui::Ui& ui, const std::string& id, float x, float y, float width,
                     float height, std::size_t lines, const AppTheme& theme) {
    ui.rect(id + ".bg")
        .position(x, y).size(width, height)
        .color(components::theme::withAlpha(theme.components.surface, 0.45f))
        .build();
    ui.rect(id + ".divider")
        .position(x + width - kEditorDividerHeight, y)
        .size(kEditorDividerHeight, height)
        .color(components::theme::withOpacity(theme.components.border, 0.45f))
        .build();
    for (std::size_t i = 0; i < lines; ++i) {
        ui.text(id + "." + std::to_string(i))
            .position(x + 4.0f, y + static_cast<float>(i) * kFontMono * 1.2f)
            .size(width - 8.0f, kFontMono * 1.2f)
            .text(std::to_string(i + 1)).fontFamily("monospace")
            .fontSize(kFontMono).lineHeight(kFontMono * 1.2f)
            .color(theme.metaText).horizontalAlign(core::HorizontalAlign::Right)
            .build();
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
    if (g_globalCookieDrafts.empty()) g_globalCookieDrafts = g_requests.globalCookies();
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
                                    .radius(10.0f)
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
                .onClick([&cookies] { cookies.push_back({g_nextTempGlobalCookieId--, {}, {}, true}); }).build();
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
                    drawParamEditor(cu, "editor.params", 0, 0, nonNegative(contentWidth - 4.0f),
                                     draft.params, theme);
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
                    drawKvEditor(cu, "editor.headers", 0, 0, nonNegative(contentWidth - 4.0f), items, theme);
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
                        drawKvEditor(cu, "editor.cookies", 0, 0, nonNegative(contentWidth - 4.0f), items, theme);
                    }).build();
            }
            if (h >= 26.0f) {
                components::button(ui, "editor.cookies.global")
                    .position(x, y + nonNegative(h - 26.0f)).size(150.0f, 24.0f)
                    .icon(0xF013).text("管理全局 Cookies").fontSize(kFontLabel)
                    .theme(tokens, false)
                    .radius(kButtonRadius)
                    .onClick([] { g_globalCookieOpen = true; }).build();
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
                        .onChange([](bool value) { activeDraft().followRedirects = value; }).build();
                }).build();
            ui.stack("editor.settings.json.comments.wrap")
                .position(x, y + 66.0f).size(w, 24.0f)
                .content([&] {
                    components::checkbox(ui, "editor.settings.json.comments")
                        .size(w, 24.0f)
                        .text("兼容带注释的 JSON").checked(draft.allowJsonComments)
                        .theme(tokens)
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
                            drawParamEditor(cu, "editor.body.form", 0, 0,
                                             nonNegative(contentWidth - 4.0f), fields, theme);
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
                drawLineNumbers(ui, "editor.body.lines", x,
                                y + selectorH + 6.0f + bodyToolbarH,
                                kEditorGutterWidth, bodyH, lineCount(bodyText), theme);
                components::input(ui, "editor.body")
                    .position(x + kEditorGutterWidth, y + selectorH + 6.0f + bodyToolbarH)
                    .size(nonNegative(w - kEditorGutterWidth), bodyH)
                    .value(bodyText).placeholder(placeholder).multiline()
                    .fontFamily("monospace").theme(tokens)
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

    ui.rect("resp.panel")
        .position(x, cy)
        .size(w, ch)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.35f : 0.6f))
        .radius(kPanelRadius)
        .build();

    const float scrollW = nonNegative(w - 8.0f);
    const float scrollH = nonNegative(ch - 8.0f);
    if (scrollW <= 0.0f || scrollH <= 0.0f) return;

    components::scrollView(ui, "resp.scroll")
        .position(x + 4.0f, cy + 4.0f)
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
    const float urlW = toolbarTwoRows
        ? nonNegative(w - methodW - kGap)
        : nonNegative(w - methodW - sendW - saveW - kGap * 3.0f);
    const float targetW = methodW + kGap + urlW;
    drawIslandPanel(ui, "req.target.island", x, y, targetW, kInputHeight, theme,
                    theme.dark ? 0.52f : 0.74f);
    const float actionsY = toolbarTwoRows ? y + kInputHeight + kGap : y;
    const float actionsX = toolbarTwoRows ? x : x + targetW + kGap;
    drawIslandPanel(ui, "req.actions.island", actionsX, actionsY,
                    sendW + saveW + kGap, kInputHeight, theme,
                    theme.dark ? 0.44f : 0.66f);

    ui.stack("req.method.wrap")
        .position(x, y)
        .size(methodW, kInputHeight)
        .zIndex(30)  // 弹层要压在下方编辑器/响应区之上
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
    const float urlX = x + methodW + kGap;
    components::input(ui, "req.url")
        .position(urlX, y)
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
            .position(x, y + toolbarH + 2.0f)
            .size(w, 14.0f)
            .text("→ " + (finalUrl.empty() ? std::string("（输入相对路径）") : finalUrl))
            .fontSize(kFontLabel)
            .fontFamily("monospace")
            .lineHeight(14.0f)
            .color(theme.hintText)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }

    // ---- 第 2 行：Params/Headers/Body 切换 + 元信息 ----
    const float tabY = y + toolbarH + 18.0f;
    const float tabWidth = std::min(430.0f, w);
    ui.stack("req.tabs.wrap")
        .position(x, tabY)
        .size(tabWidth, 24.0f)
        .content([&] {
            components::segmented(ui, "req.tabs")
                .size(tabWidth, 24.0f)
                .items({"Params", "Headers", "Body", "Cookies", "设置"})
                .selected(static_cast<int>(draft.tab))
                .fontSize(kFontLabel)
                .theme(tokens)
                .style(segmentedStyle(theme))
                .onChange([](int i) {
                    activeDraft().tab = static_cast<EditorTab>(std::clamp(i, 0, 4));
                    persistSessionState();
                })
                .build();
        })
        .build();

    const float metaX = x + tabWidth + kGap;
    const float metaW = x + w - metaX;
    if (metaW > 60.0f) {
        ui.text("req.tab.meta")
            .position(metaX, tabY)
            .size(metaW, 24.0f)
            .text(std::format("{} 个参数 · {} 个请求头 · {} 个 Cookie", draft.params.size(), draft.headers.size(), draft.cookies.size()))
            .fontSize(kFontLabel)
            .lineHeight(24.0f)
            .color(theme.hintText)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }

    // ---- 编辑器区（上 42%）/ 响应区（下方剩余）：高度全部从可用空间推导， ----
    // 不再用 min 80 把响应区推出页面。岛屿精确覆盖，内容内缩 kPanelPad。
    const float editorY = tabY + 30.0f;
    const float splitH = nonNegative(y + h - editorY);
    const float editorH = nonNegative(splitH * 0.42f);
    drawIslandPanel(ui, "req.tabs.island", x, tabY, w, 24.0f, theme,
                    theme.dark ? 0.50f : 0.72f);
    const float respY = editorY + editorH + kGap;
    const float respH = nonNegative(y + h - respY);
    drawIslandPanel(ui, "req.response.island", x, respY, w, respH, theme,
                    theme.dark ? 0.64f : 0.84f);
    if (editorH > 0.0f) {
        drawEditor(ui, x + kPanelPad, editorY + kPanelPad, nonNegative(w - kPanelPad * 2.0f),
                   nonNegative(editorH - kPanelPad), theme);
    }
    if (respH > 0.0f) {
        drawResponse(ui, x + kPanelPad, respY + kPanelPad, nonNegative(w - kPanelPad * 2.0f),
                     nonNegative(respH - kPanelPad), theme);
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
        g_envDraftProjectId = g_requests.currentProjectId();
    }
    for (const auto& e : envs) {
        g_envNameDrafts.try_emplace(e.id, e.name);
        g_envUrlDrafts.try_emplace(e.id, e.baseUrl);
    }

    constexpr float kRowH = 30.0f;
    // 高度有界：优先按环境行数，dialogHeight clamp 到屏幕；列表区滚动。
    const float dlgW = dialogWidth(screen.width, 460.0f);
    const float dlgH = dialogHeight(screen.height,
        166.0f + static_cast<float>(std::max<int>(1, envs.size())) * kRowH);
    const float pad = 16.0f;
    const float btnW = 74.0f;
    const float btnH = 24.0f;
    const float footerY = nonNegative(dlgH - btnH - 16.0f);
    const float addY = nonNegative(footerY - 28.0f);
    const float listY = 52.0f;
    const float listH = nonNegative(addY - listY - 4.0f);

    components::dialog(ui, "req.env.dialog")
        .open(true)
        .screen(screen.width, screen.height)
        .size(dlgW, dlgH)
        .title("环境管理（当前项目）")
        .theme(tokens)
        .content([&] {
            if (listH > 0.0f) {
                components::scrollView(ui, "req.env.scroll")
                    .position(pad, listY)
                    .size(nonNegative(dlgW - pad * 2.0f), listH)
                    .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                    .theme(tokens)
                    .content([&](eui::Ui& cu, float contentWidth, float) {
                        const float rowW = nonNegative(contentWidth);
                        const float delW = 20.0f;
                        const float nameW = std::min(110.0f, nonNegative(rowW - delW - 12.0f) * 0.32f);
                        const float urlW = nonNegative(rowW - nameW - delW - 12.0f);
                        if (envs.empty()) {
                            cu.text("req.env.dialog.empty")
                                .position(0, 0)
                                .size(rowW, 20.0f)
                                .text("还没有环境，点下方「新建环境」")
                                .fontSize(kFontLabel)
                                .color(theme.hintText)
                                .build();
                        }
                        for (const auto& e : envs) {
                            const std::string base = "req.env.dialog." + std::to_string(e.id);
                            cu.stack(base).size(rowW, kRowH).content([&] {
                                components::input(cu, base + ".name")
                                    .position(0, 0)
                                    .size(nameW, 24.0f)
                                    .value(g_envNameDrafts[e.id])
                                    .placeholder("名称")
                                    .theme(tokens)
                                    .onChange([id = e.id](const std::string& v) {
                                        g_envNameDrafts[id] = v;
                                    })
                                    .build();
                                components::input(cu, base + ".url")
                                    .position(nameW + 6.0f, 0)
                                    .size(urlW, 24.0f)
                                    .value(g_envUrlDrafts[e.id])
                                    .placeholder("http://localhost:8080/")
                                    .fontFamily("monospace")
                                    .fontSize(kFontMono)
                                    .theme(tokens)
                                    .onChange([id = e.id](const std::string& v) {
                                        g_envUrlDrafts[id] = v;
                                    })
                                    .build();
                                components::button(cu, base + ".del")
                                    .position(nonNegative(rowW - delW), 2.0f)
                                    .size(delW, delW)
                                    .icon(0xF1F8)  // fa-trash
                                    .text("")
                                    .iconSize(9.0f)
                                    .theme(tokens, false)
                                    .radius(10.0f)
                                    .onClick([id = e.id, name = e.name] {
                                        askConfirm("删除环境",
                                                   std::format("将删除环境「{}」。", name),
                                                   [id] {
                                                       const std::string err = g_requests.deleteEnvironment(id);
                                                       showStatus(err.empty() ? "环境已删除"
                                                                              : ("删除失败: " + err));
                                                   });
                                    })
                                    .build();
                            }).build();
                        }
                    })
                    .build();
            }
            components::button(ui, "req.env.dialog.add")
                .position(pad, addY)
                .size(90.0f, 22.0f)
                .icon(0xF067)  // fa-plus
                .text("新建环境")
                .fontSize(kFontLabel)
                .iconSize(8.0f)
                .theme(tokens, false)
                .radius(kButtonRadius)
                .onClick([] {
                    (void)g_requests.createEnvironment("新环境", "http://localhost:8080/");
                })
                .build();
            const float saveX = nonNegative(dlgW - pad - btnW);
            const float cancelX = nonNegative(saveX - 8.0f - btnW);
            components::button(ui, "req.env.dialog.cancel")
                .position(cancelX, footerY)
                .size(btnW, btnH)
                .text("取消")
                .fontSize(kFontLabel)
                .theme(tokens, false)
                .radius(kButtonRadius)
                .onClick([] {
                    g_envManageOpen = false;
                    g_envNameDrafts.clear();
                    g_envUrlDrafts.clear();
                })
                .build();
            components::button(ui, "req.env.dialog.save")
                .position(saveX, footerY)
                .size(btnW, btnH)
                .text("保存")
                .fontSize(kFontLabel)
                .theme(tokens, true)
                .radius(kButtonRadius)
                .textColor(onPrimaryColor(theme))
                .iconColor(onPrimaryColor(theme))
                .onClick([envs] {
                    for (const auto& e : envs) {
                        const std::string name = trim(g_envNameDrafts[e.id]);
                        if (name.empty()) {
                            showStatus("环境名称不能为空");
                            return;
                        }
                        if (const std::string err = g_requests.updateEnvironment(
                                e.id, name, trim(g_envUrlDrafts[e.id]));
                            !err.empty()) {
                            showStatus("保存环境失败: " + err);
                            return;
                        }
                    }
                    g_envManageOpen = false;
                    g_envNameDrafts.clear();
                    g_envUrlDrafts.clear();
                })
                .build();
        })
        .build();
}
