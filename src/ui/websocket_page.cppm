// ui/websocket_page.cppm — WebSocket 专用连接与消息页面。
module;

#include "eui_ui.h"

export module apitab.ui.websocket_page;

import std;
import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.store.ui;
import apitab.store.websocket;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

const char* stateLabel(api::WebSocketState state) {
    switch (state) {
        case api::WebSocketState::Disconnected: return "未连接";
        case api::WebSocketState::Connecting: return "连接中";
        case api::WebSocketState::Connected: return "已连接";
        case api::WebSocketState::Failed: return "连接失败";
    }
    return "未知";
}

std::string websocketUrl(const Draft& draft) {
    const std::string raw = trim(g_requests.composeUrl(draft.url, draft.groupId,
                                                       g_requests.currentEnvId()));
    if (raw.starts_with("http://")) return "ws://" + raw.substr(7);
    if (raw.starts_with("https://")) return "wss://" + raw.substr(8);
    if (!hasUriScheme(raw)) return "ws://" + raw;
    return raw;
}

void saveWebSocketRequest() {
    RequestTab& tab = activeTab();
    db::SavedRequest request{
        .id = tab.requestId,
        .groupId = tab.draft.groupId,
        .name = tab.draft.name,
        .kind = api::RequestKind::WebSocket,
        .wsProtocol = tab.draft.wsProtocol,
        .method = "GET",
        .url = tab.draft.url,
        .headers = tab.draft.headers,
    };
    const std::string error = g_requests.save(request);
    if (!error.empty()) { showStatus("保存失败: " + error); return; }
    tab.requestId = request.id;
    showStatus("已保存: " + request.name);
}

} // namespace

export void drawWebSocketPage(eui::Ui& ui, float x, float y, float w, float h,
                               const AppTheme& theme) {
    const auto& tokens = theme.components;
    RequestTab& tab = activeTab();
    Draft& draft = tab.draft;
    const api::WebSocketState state = g_websocket.state(tab.uid);
    tab.wsState = state;
    const bool connected = state == api::WebSocketState::Connected ||
                           state == api::WebSocketState::Connecting;

    constexpr float connectW = 84.0f;
    constexpr float saveW = 52.0f;
    constexpr float protocolW = 150.0f;
    const float urlW = std::max(140.0f, w - connectW - saveW - protocolW - kGap * 3.0f);
    components::input(ui, "ws.url")
        .position(x, y).size(urlW, kInputHeight).value(draft.url)
        .placeholder("ws://localhost:8080/socket").fontFamily("monospace").theme(tokens)
        .onChange([](const std::string& value) { activeDraft().url = value; }).build();
    components::input(ui, "ws.protocol")
        .position(x + urlW + kGap, y).size(protocolW, kInputHeight).value(draft.wsProtocol)
        .placeholder("子协议（可选）").theme(tokens)
        .onChange([](const std::string& value) { activeDraft().wsProtocol = value; }).build();
    components::button(ui, "ws.connect")
        .position(x + urlW + protocolW + kGap * 2.0f, y).size(connectW, kButtonHeight)
        .icon(connected ? 0xF04D : 0xF1D8).text(connected ? "断开" : "连接")
        .fontSize(kFontLabel).theme(tokens, true)
        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
        .onClick([connected] {
            RequestTab& active = activeTab();
            if (connected) {
                g_websocket.disconnect(active.uid);
                return;
            }
            const std::string url = websocketUrl(active.draft);
            if (url.empty()) { showStatus("WebSocket 地址不能为空"); return; }
            const std::string error = g_websocket.connect(active.uid, api::WebSocketSpec{
                .url = url, .headers = active.draft.headers, .subprotocol = active.draft.wsProtocol,
            });
            showStatus(error.empty() ? "WebSocket 连接中…" : error);
        }).build();
    components::button(ui, "ws.save")
        .position(x + urlW + protocolW + connectW + kGap * 3.0f, y).size(saveW, kButtonHeight)
        .icon(0xF0C7).text("保存").fontSize(kFontLabel).theme(tokens, false)
        .onClick([] { saveWebSocketRequest(); }).build();

    ui.text("ws.state")
        .position(x, y + 32.0f).size(w, 18.0f)
        .text(std::string("WebSocket · ") + stateLabel(state) + " · " + websocketUrl(draft))
        .fontSize(kFontLabel).lineHeight(18.0f)
        .color(state == api::WebSocketState::Failed ? theme.serverErr : theme.metaText)
        .verticalAlign(core::VerticalAlign::Center).build();

    const float composerH = 58.0f;
    const float timelineY = y + 56.0f;
    const float timelineH = std::max(70.0f, h - 56.0f - composerH - kGap);
    drawIslandPanel(ui, "ws.timeline.island", x, timelineY, w, timelineH, theme,
                    theme.dark ? 0.68f : 0.86f);
    components::scrollView(ui, "ws.timeline.scroll")
        .position(x + 4.0f, timelineY + 4.0f).size(w - 8.0f, timelineH - 8.0f)
        .offset(tab.wsScroll).theme(tokens)
        .onChange([](float value) { activeTab().wsScroll = value; })
        .content([&](eui::Ui& content, float contentWidth, float) {
            if (tab.wsEvents.empty()) {
                content.stack("ws.timeline.empty.row").size(contentWidth, 32.0f).content([&] {
                    content.text("ws.timeline.empty").position(8.0f, 8.0f)
                        .size(contentWidth - 16.0f, 18.0f).text("连接后将在此显示事件和消息")
                        .fontSize(kFontLabel).color(theme.hintText).build();
                }).build();
            }
            for (int i = 0; i < static_cast<int>(tab.wsEvents.size()); ++i) {
                const auto& event = tab.wsEvents[i];
                const std::string row = "ws.timeline." + std::to_string(i);
                std::string prefix;
                switch (event.kind) {
                    case api::WebSocketEventKind::Open: prefix = "OPEN"; break;
                    case api::WebSocketEventKind::Text: prefix = "← TEXT"; break;
                    case api::WebSocketEventKind::Binary: prefix = "← BINARY"; break;
                    case api::WebSocketEventKind::Close: prefix = "CLOSE"; break;
                    case api::WebSocketEventKind::Error: prefix = "ERROR"; break;
                }
                const std::string text = prefix + "  " +
                    (!event.payload.empty() ? event.payload : event.detail);
                content.stack(row).size(contentWidth, 22.0f).content([&] {
                    content.text(row + ".text").position(8.0f, 2.0f)
                        .size(contentWidth - 16.0f, 18.0f).text(text).fontFamily("monospace")
                        .fontSize(kFontMono)
                        .color(event.kind == api::WebSocketEventKind::Error ? theme.serverErr : theme.bodyText)
                        .wrap(true).build();
                }).build();
            }
        }).build();

    const float composerY = timelineY + timelineH + kGap;
    components::input(ui, "ws.message")
        .position(x, composerY).size(std::max(80.0f, w - 126.0f), composerH)
        .value(tab.wsMessage).placeholder("输入要发送的消息").multiline()
        .fontFamily("monospace").theme(tokens)
        .onChange([](const std::string& value) { activeTab().wsMessage = value; }).build();
    ui.stack("ws.mode.wrap").position(x + w - 120.0f, composerY).size(120.0f, 24.0f)
        .content([&] {
            components::segmented(ui, "ws.mode").size(120.0f, 24.0f)
                .items({"文本", "二进制"}).selected(tab.wsBinary ? 1 : 0).theme(tokens)
                .style(segmentedStyle(theme)).onChange([](int index) { activeTab().wsBinary = index == 1; }).build();
        }).build();
    components::button(ui, "ws.send")
        .position(x + w - 120.0f, composerY + 32.0f).size(120.0f, 26.0f)
        .icon(0xF1D8).text("发送消息").fontSize(kFontLabel).theme(tokens, true)
        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
        .onClick([] {
            RequestTab& active = activeTab();
            const std::string error = g_websocket.send(active.uid, active.wsMessage, active.wsBinary);
            if (!error.empty()) { showStatus(error); return; }
            active.wsEvents.push_back(api::WebSocketEvent{
                .kind = active.wsBinary ? api::WebSocketEventKind::Binary : api::WebSocketEventKind::Text,
                .payload = "→ " + active.wsMessage,
            });
            active.wsMessage.clear();
        }).build();
}
