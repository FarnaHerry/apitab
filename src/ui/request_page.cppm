// ui/request_page.cppm — 请求页：URL 编辑行 + Params/Headers/Body 编辑器 + 响应查看。
// 编辑对象是「当前请求标签页」的草稿（activeDraft），响应也按 tab 持有。
module;

#include "eui_ui.h"

export module apitab.ui.request_page;

import std;
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

// 最终 URL = 环境 base + Path 分组前缀 + 相对路径（由领域 store 统一拼）。
std::string composeFinalUrl(const Draft& draft) {
    return g_requests.composeUrl(draft.url, draft.groupId, g_requests.currentEnvId());
}

void sendCurrentRequest() {
    RequestTab& tab = activeTab();
    const std::string finalUrl = composeFinalUrl(tab.draft);
    if (finalUrl.empty()) {
        showStatus("URL 不能为空");
        return;
    }
    api::RequestSpec spec = buildSpec(tab.draft, finalUrl);
    if (spec.url.find("://") == std::string::npos) {
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
        .method = kMethods[tab.draft.methodIndex],
        .url = tab.draft.url,
        .params = tab.draft.params,
        .headers = tab.draft.headers,
        .bodyKind = tab.draft.bodyKind,
        .body = tab.draft.body,
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
    components::dialog(ui, "req.save.name.dialog")
        .open(true)
        .screen(screen.width, screen.height)
        .size(360.0f, 154.0f)
        .title("保存请求")
        .theme(theme.components)
        .content([&] {
            components::input(ui, "req.save.name.input")
                .position(20.0f, 56.0f)
                .size(320.0f, kInputHeight)
                .value(g_saveRequestNameText)
                .placeholder("请求名称")
                .theme(theme.components)
                .onChange([](const std::string& v) { g_saveRequestNameText = v; })
                .build();
            components::button(ui, "req.save.name.cancel")
                .position(184.0f, 104.0f)
                .size(74.0f, 24.0f)
                .text("取消")
                .fontSize(kFontLabel)
                .theme(theme.components, false)
                .onClick([] {
                    g_saveRequestNameOpen = false;
                    g_saveRequestNameText.clear();
                })
                .build();
            components::button(ui, "req.save.name.confirm")
                .position(266.0f, 104.0f)
                .size(74.0f, 24.0f)
                .text("保存")
                .fontSize(kFontLabel)
                .theme(theme.components, true)
                .textColor(onPrimaryColor(theme))
                .iconColor(onPrimaryColor(theme))
                .onClick([] {
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
                })
                .build();
        })
        .build();
}

// ---- 编辑器区（按 tab 分发）----

void drawEditor(eui::Ui& ui, float x, float y, float w, float h, const AppTheme& theme) {
    const auto& tokens = theme.components;
    Draft& draft = activeDraft();
    switch (draft.tab) {
        case EditorTab::Params: {
            components::scrollView(ui, "editor.params.scroll")
                .position(x, y).size(w, h).offset(g_editorScroll).theme(tokens)
                .onChange([](float v) { g_editorScroll = v; })
                .content([&](eui::Ui& cu, float contentWidth, float) {
                    drawParamEditor(cu, "editor.params", 0, 0, contentWidth - 4.0f,
                                     draft.params, theme);
                })
                .build();
            break;
        }
        case EditorTab::Headers: {
            auto& items = draft.headers;
            components::scrollView(ui, "editor.headers.scroll")
                .position(x, y).size(w, h).offset(g_editorScroll).theme(tokens)
                .onChange([](float v) { g_editorScroll = v; })
                .content([&](eui::Ui& cu, float contentWidth, float) {
                    drawKvEditor(cu, "editor.headers", 0, 0, contentWidth - 4.0f, items, theme);
                })
                .build();
            break;
        }
        case EditorTab::Body: {
            constexpr float selectorH = 24.0f;
            ui.stack("editor.body.kind.wrap")
                .position(x, y).size(250.0f, selectorH)
                .content([&] {
                    components::segmented(ui, "editor.body.kind")
                        .size(250.0f, selectorH)
                        .items({"None", "JSON", "Text", "Form"})
                        .selected(static_cast<int>(draft.bodyKind))
                        .theme(tokens).style(segmentedStyle(theme))
                        .onChange([](int i) {
                            activeDraft().bodyKind = static_cast<api::BodyKind>(
                                std::clamp(i, 0, 3));
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
                const char* placeholder = draft.bodyKind == api::BodyKind::Json
                    ? "JSON 请求体"
                    : draft.bodyKind == api::BodyKind::FormUrlEncoded
                        ? "表单内容，例如 name=apitab&mode=test"
                        : "文本请求体";
                components::input(ui, "editor.body")
                    .position(x, y + selectorH + 6.0f)
                    .size(w, std::max(40.0f, h - selectorH - 6.0f))
                    .value(draft.body).placeholder(placeholder).multiline()
                    .fontFamily("monospace").theme(tokens)
                    .onChange([](const std::string& v) { activeDraft().body = v; })
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
                .onChange([](int i) { g_responseTab = i == 0 ? ResponseTab::Body : ResponseTab::Headers; })
                .build();
        })
        .build();

    // 内容区
    const float cy = y + 28.0f;
    const float ch = h - 28.0f;
    if (!tab.hasResponse) return;

    ui.rect("resp.panel")
        .position(x, cy)
        .size(w, ch)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.35f : 0.6f))
        .radius(kPanelRadius)
        .build();

    components::scrollView(ui, "resp.scroll")
        .position(x + 4.0f, cy + 4.0f)
        .size(w - 8.0f, ch - 8.0f)
        .offset(tab.bodyScroll)
        .theme(tokens)
        .onChange([](float v) { activeTab().bodyScroll = v; })
        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
            if (g_responseTab == ResponseTab::Body) {
                cu.text("resp.body")
                    .position(4.0f, 4.0f)
                    .size(contentWidth - 8.0f, viewportH)
                    .text(tab.response.body.empty() ? "(空响应体)" : tab.response.body)
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
                    cu.stack(rowId)
                        .size(contentWidth - 8.0f, 16.0f)
                        .content([&] {
                            cu.text(rowId + ".k")
                                .position(0, 0)
                                .size(contentWidth * 0.34f, 16.0f)
                                .text(hv.key)
                                .fontSize(kFontMono)
                                .color(theme.metaText)
                                .build();
                            cu.text(rowId + ".v")
                                .position(contentWidth * 0.34f + 10.0f, 0)
                                .size(contentWidth * 0.66f - 14.0f, 16.0f)
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
                        .size(contentWidth - 8.0f, 16.0f)
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

    // ---- 第 1 行：method + 环境 + ⚙ + URL（弹性）+ 发送 + 保存 ----
    const float sendW = 64.0f, saveW = 52.0f, methodW = 96.0f, envW = 170.0f, gearW = 26.0f;
    const float urlW = std::max(120.0f, w - methodW - envW - gearW - sendW - saveW - kGap * 5.0f);

    ui.stack("req.method.wrap")
        .position(x, y)
        .size(methodW, kInputHeight)
        .zIndex(20)  // 弹层要压在下方编辑器/响应区之上
        .content([&] {
            components::dropdown(ui, "req.method")
                .size(methodW, kInputHeight)
                .items(kMethods)
                .selected(draft.methodIndex)
                .open(g_methodOpen)
                .theme(tokens)
                .onOpenChange([](bool o) { g_methodOpen = o; })
                .onChange([](int i) { activeDraft().methodIndex = i; })
                .build();
        })
        .build();

    // 环境选择（dropdown：显示环境名，首个为「未选择」；同时展示 base URL）
    const auto& envs = g_requests.environments();
    std::vector<std::string> envNames;
    int envSelected = 0;
    for (int i = 0; i < static_cast<int>(envs.size()); ++i) {
        std::string label = envs[i].name;
        if (!envs[i].baseUrl.empty()) label += "  " + envs[i].baseUrl;
        envNames.push_back(std::move(label));
        if (envs[i].id == g_requests.currentEnvId()) envSelected = i + 1;  // +1 留给「未选择」
    }
    envNames.insert(envNames.begin(), "未选择环境");

    ui.stack("req.env.wrap")
        .position(x + methodW + kGap, y)
        .size(envW, kInputHeight)
        .zIndex(19)  // 弹层要压在下方编辑器/响应区之上（低于 method 的 20）
        .content([&] {
            components::dropdown(ui, "req.env")
                .size(envW, kInputHeight)
                .items(envNames)
                .selected(envSelected)
                .open(g_envOpen)
                .theme(tokens)
                .onOpenChange([](bool o) { g_envOpen = o; })
                .onChange([envs](int i) {
                    (void)g_requests.selectEnv(i > 0 ? envs[i - 1].id : 0);
                })
                .build();
        })
        .build();

    // 环境管理弹窗开关（⚙）
    components::button(ui, "req.env.manage")
        .position(x + methodW + kGap + envW + kGap, y)
        .size(gearW, kInputHeight)
        .icon(0xF013)  // fa-gear
        .text("")
        .iconSize(10.0f)
        .theme(tokens, false)
        .onClick([] { g_envManageOpen = true; })
        .build();

    const bool busy = g_requests.busy();
    const float urlX = x + methodW + envW + gearW + kGap * 3.0f;
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
        .position(urlX + urlW + kGap, y)
        .size(sendW, kButtonHeight)
        .icon(busy ? 0xF04D : 0xF1D8)  // 发送中显示停止块（点击=取消）
        .text(busy ? "取消" : "发送")
        .fontSize(kFontBody)
        .theme(tokens, true)
        .textColor(onPrimaryColor(theme))   // 白底主色 → 深色字/图标才可读
        .iconColor(onPrimaryColor(theme))
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
        .position(urlX + urlW + kGap + sendW + kGap, y)
        .size(saveW, kButtonHeight)
        .icon(0xF0C7)  // fa-floppy-disk
        .text("保存")
        .fontSize(kFontBody)
        .theme(tokens, false)
        .onClick([] { saveCurrentRequest(); })
        .build();

    // ---- 第 1.5 行：最终 URL 预览（环境 / 路径分组前缀生效时显示完整目标）----
    const std::string finalUrl = composeFinalUrl(draft);
    if (finalUrl != draft.url) {
        ui.text("req.url.preview")
            .position(x, y + kInputHeight + 2.0f)
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
    const float tabY = y + kInputHeight + 18.0f;
    ui.stack("req.tabs.wrap")
        .position(x, tabY)
        .size(230.0f, 24.0f)
        .content([&] {
            components::segmented(ui, "req.tabs")
                .size(230.0f, 24.0f)
                .items({"Params", "Headers", "Body"})
                .selected(static_cast<int>(draft.tab))
                .fontSize(kFontLabel)
                .theme(tokens)
                .style(segmentedStyle(theme))
                .onChange([](int i) { activeDraft().tab = static_cast<EditorTab>(i); })
                .build();
        })
        .build();

    const float metaX = x + 240.0f;
    const float metaW = x + w - metaX;
    if (metaW > 60.0f) {
        ui.text("req.tab.meta")
            .position(metaX, tabY)
            .size(metaW, 24.0f)
            .text(std::format("{} 个参数 · {} 个请求头", draft.params.size(), draft.headers.size()))
            .fontSize(kFontLabel)
            .lineHeight(24.0f)
            .color(theme.hintText)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }

    // ---- 编辑器区（上 45%）----
    const float editorY = tabY + 30.0f;
    const float editorH = std::max(80.0f, (y + h - editorY) * 0.42f);
    drawEditor(ui, x, editorY, w, editorH, theme);

    // ---- 响应区（下方剩余）----
    const float respY = editorY + editorH + kGap;
    drawResponse(ui, x, respY, w, y + h - respY, theme);
}

// 环境管理弹窗（需要全屏坐标系，由 app.cpp 传入 screen）。
// 列表内直接编辑名称 / baseUrl（即时落库），＋ 新建，🗑 删除（弹确认）。
export void drawRequestPageDialogs(eui::Ui& ui, const eui::Screen& screen,
                                   const AppTheme& theme) {
    drawSaveRequestNameDialog(ui, screen, theme);
    if (!g_envManageOpen) return;
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
    const float dlgW = 460.0f;
    const float dlgH = 166.0f + static_cast<float>(std::max<int>(1, envs.size())) * kRowH;

    components::dialog(ui, "req.env.dialog")
        .open(true)
        .screen(screen.width, screen.height)
        .size(dlgW, dlgH)
        .title("环境管理（当前项目）")
        .theme(tokens)
        .content([&] {
            float ry = 52.0f;
            if (envs.empty()) {
                ui.text("req.env.dialog.empty")
                    .position(16.0f, ry)
                    .size(dlgW - 32.0f, 20.0f)
                    .text("还没有环境，点下方「新建环境」")
                    .fontSize(kFontLabel)
                    .color(theme.hintText)
                    .build();
                ry += kRowH;
            }
            for (const auto& e : envs) {
                const std::string base = "req.env.dialog." + std::to_string(e.id);
                components::input(ui, base + ".name")
                    .position(16.0f, ry)
                    .size(110.0f, 24.0f)
                    .value(g_envNameDrafts[e.id])
                    .placeholder("名称")
                    .theme(tokens)
                    .onChange([id = e.id](const std::string& v) {
                        g_envNameDrafts[id] = v;
                    })
                    .build();
                components::input(ui, base + ".url")
                    .position(132.0f, ry)
                    .size(dlgW - 132.0f - 56.0f, 24.0f)
                    .value(g_envUrlDrafts[e.id])
                    .placeholder("http://localhost:8080/")
                    .fontFamily("monospace")
                    .fontSize(kFontMono)
                    .theme(tokens)
                    .onChange([id = e.id](const std::string& v) {
                        g_envUrlDrafts[id] = v;
                    })
                    .build();
                components::button(ui, base + ".del")
                    .position(dlgW - 46.0f, ry + 2.0f)
                    .size(20.0f, 20.0f)
                    .icon(0xF1F8)  // fa-trash
                    .text("")
                    .iconSize(9.0f)
                    .theme(tokens, false)
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
                ry += kRowH;
            }
            components::button(ui, "req.env.dialog.add")
                .position(16.0f, ry + 2.0f)
                .size(90.0f, 22.0f)
                .icon(0xF067)  // fa-plus
                .text("新建环境")
                .fontSize(kFontLabel)
                .iconSize(8.0f)
                .theme(tokens, false)
                .onClick([] {
                    (void)g_requests.createEnvironment("新环境", "http://localhost:8080/");
                })
                .build();
            components::button(ui, "req.env.dialog.cancel")
                .position(dlgW - 176.0f, ry + 36.0f)
                .size(74.0f, 24.0f)
                .text("取消")
                .fontSize(kFontLabel)
                .theme(tokens, false)
                .onClick([] {
                    g_envManageOpen = false;
                    g_envNameDrafts.clear();
                    g_envUrlDrafts.clear();
                })
                .build();
            components::button(ui, "req.env.dialog.save")
                .position(dlgW - 94.0f, ry + 36.0f)
                .size(74.0f, 24.0f)
                .text("保存")
                .fontSize(kFontLabel)
                .theme(tokens, true)
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
