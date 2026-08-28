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

    // ---- 工具栏：单行最小跨度 444（url140+protocol150+connect84+save52+gaps18），
    // 低于阈值换两行：URL 独占一行，protocol/connect/save 第二行。宽度全部非负。
    constexpr float connectW = 84.0f;
    constexpr float saveW = 52.0f;
    constexpr float protocolW = 150.0f;
    const bool twoRows = w < 444.0f;
    const float toolbarH = twoRows ? kInputHeight * 2.0f + kGap : kInputHeight;
    const std::string id = "ws." + std::to_string(tab.uid);
    drawIsland(ui, id + ".toolbar.island", x, y, w, toolbarH,
               theme, theme.dark ? 0.56f : 0.78f, [&] {

    const float urlW = twoRows
        ? w
        : nonNegative(w - connectW - saveW - protocolW - kGap * 3.0f);
    components::input(ui, id + ".url")
        .position(0, 0).size(urlW, kInputHeight).value(draft.url)
        .placeholder("ws://localhost:8080/socket").fontFamily("monospace").theme(tokens)
        .onChange([](const std::string& value) { activeDraft().url = value; }).build();

    const float row2Y = twoRows ? kInputHeight + kGap : 0;
    const float protoW = twoRows
        ? std::min(protocolW, nonNegative(w - connectW - saveW - kGap * 2.0f))
        : protocolW;
    const float protoX = twoRows ? 0 : urlW + kGap;
    const float connectX = twoRows ? protoW + kGap
                                   : urlW + protocolW + kGap * 2.0f;
    const float saveX = twoRows ? protoW + connectW + kGap * 2.0f
                                : urlW + protocolW + connectW + kGap * 3.0f;
    components::input(ui, id + ".protocol")
        .position(protoX, row2Y).size(protoW, kInputHeight).value(draft.wsProtocol)
        .placeholder("子协议（可选）").theme(tokens)
        .onChange([](const std::string& value) { activeDraft().wsProtocol = value; }).build();
    components::button(ui, id + ".connect")
        .position(connectX, row2Y).size(connectW, kButtonHeight)
        .icon(connected ? 0xF04D : 0xF1D8).text(connected ? "断开" : "连接")
        .fontSize(kFontLabel).theme(tokens, true)
        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
        .radius(kButtonRadius)
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
    components::button(ui, id + ".save")
        .position(saveX, row2Y).size(saveW, kButtonHeight)
        .icon(0xF0C7).text("保存").fontSize(kFontLabel).theme(tokens, false)
        .radius(kButtonRadius)
        .onClick([] { saveWebSocketRequest(); }).build();
    });

    // 状态行在短窗口让位（时间线/composer 优先保住）。
    const bool showState = h >= 170.0f;
    if (showState) {
        ui.text(id + ".state")
            .position(x, y + toolbarH + 6.0f).size(w, 18.0f)
            .text(std::string("WebSocket · ") + stateLabel(state) + " · " + websocketUrl(draft))
            .fontSize(kFontLabel).lineHeight(18.0f)
            .color(state == api::WebSocketState::Failed ? theme.serverErr : theme.metaText)
            .verticalAlign(core::VerticalAlign::Center).build();
    }

    // ---- 时间线 + composer：composer 高度固定，时间线吃掉剩余（不为负）。----
    const bool narrowComposer = w < 252.0f;
    const float composerH = narrowComposer ? 56.0f : 58.0f;
    const float timelineY = y + toolbarH + (showState ? 30.0f : 6.0f);
    const float timelineH = nonNegative(h - (timelineY - y) - composerH - kIslandGap);
    drawIsland(ui, id + ".timeline.island", x, timelineY, w, timelineH, theme,
               theme.dark ? 0.68f : 0.86f, [&] {
    const float scrollW = nonNegative(w - kPanelPad * 2.0f);
    const float scrollH = nonNegative(timelineH - kPanelPad * 2.0f);
    if (scrollW > 0.0f && scrollH > 0.0f) {
        components::scrollView(ui, id + ".timeline.scroll")
            .position(kPanelPad, kPanelPad).size(scrollW, scrollH)
            .offset(tab.wsScroll).theme(tokens)
            .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
            .onChange([](float value) { activeTab().wsScroll = value; })
            .content([&](eui::Ui& content, float contentWidth, float) {
                const float rowW = nonNegative(contentWidth);
                if (tab.wsEvents.empty()) {
                    content.stack(id + ".timeline.empty.row").size(rowW, 32.0f).content([&] {
                        content.text(id + ".timeline.empty").position(8.0f, 8.0f)
                            .size(nonNegative(rowW - 16.0f), 18.0f).text("连接后将在此显示事件和消息")
                            .fontSize(kFontLabel).color(theme.hintText).build();
                    }).build();
                }
                for (int i = 0; i < static_cast<int>(tab.wsEvents.size()); ++i) {
                    const auto& event = tab.wsEvents[i];
                    const std::string row = id + ".timeline." + std::to_string(i);
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
                    // wrap 文本量出真实行高，scroll 测量与视觉一致。
                    const float textH = measureWrappedTextHeight(
                        text, nonNegative(rowW - 16.0f), kFontMono, "monospace");
                    const float rowH = std::max(22.0f, textH + 4.0f);
                    content.stack(row).size(rowW, rowH).content([&] {
                        content.text(row + ".text").position(8.0f, 2.0f)
                            .size(nonNegative(rowW - 16.0f), textH).text(text).fontFamily("monospace")
                            .fontSize(kFontMono)
                            .color(event.kind == api::WebSocketEventKind::Error ? theme.serverErr : theme.bodyText)
                            .wrap(true).build();
                    }).build();
                }
            }).build();
    }
    });

    // ---- composer：宽窗口 message 在左、控制在右；窄窗口 message 独占一行、
    // mode/send 第二行均分。右侧控件永远不越出岛屿。
    const float composerY = timelineY + timelineH + kIslandGap;
    drawIsland(ui, id + ".composer.island", x, composerY, w, composerH,
               theme, theme.dark ? 0.52f : 0.74f, [&] {
    if (narrowComposer) {
        const float halfW = nonNegative((w - kGap) * 0.5f);
        components::input(ui, id + ".message")
            .position(0, 0).size(w, 26.0f)
            .value(tab.wsMessage).placeholder("输入要发送的消息").multiline()
            .fontFamily("monospace").theme(tokens)
            .onChange([](const std::string& value) { activeTab().wsMessage = value; }).build();
        ui.stack(id + ".mode.wrap").position(0, 30.0f).size(halfW, 24.0f)
            .content([&] {
                components::segmented(ui, id + ".mode").size(halfW, 24.0f)
                    .items({"文本", "二进制"}).selected(tab.wsBinary ? 1 : 0).theme(tokens)
                    .style(segmentedStyle(theme)).onChange([](int index) { activeTab().wsBinary = index == 1; }).build();
            }).build();
        components::button(ui, id + ".send")
            .position(halfW + kGap, 30.0f).size(halfW, 26.0f)
            .icon(0xF1D8).text("发送消息").fontSize(kFontLabel).theme(tokens, true)
            .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
            .radius(kButtonRadius)
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
    } else {
        components::input(ui, id + ".message")
            .position(0, 0).size(nonNegative(w - 126.0f), composerH)
            .value(tab.wsMessage).placeholder("输入要发送的消息").multiline()
            .fontFamily("monospace").theme(tokens)
            .onChange([](const std::string& value) { activeTab().wsMessage = value; }).build();
        ui.stack(id + ".mode.wrap").position(w - 120.0f, 0).size(120.0f, 24.0f)
            .content([&] {
                components::segmented(ui, id + ".mode").size(120.0f, 24.0f)
                    .items({"文本", "二进制"}).selected(tab.wsBinary ? 1 : 0).theme(tokens)
                    .style(segmentedStyle(theme)).onChange([](int index) { activeTab().wsBinary = index == 1; }).build();
            }).build();
        components::button(ui, id + ".send")
            .position(w - 120.0f, 32.0f).size(120.0f, 26.0f)
            .icon(0xF1D8).text("发送消息").fontSize(kFontLabel).theme(tokens, true)
            .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
            .radius(kButtonRadius)
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
    });
}
