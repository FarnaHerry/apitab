// ui/tcp_page.cppm — TCP / TCPS 原始字节流测试页面。
module;

#include "eui_ui.h"

export module apitab.ui.tcp_page;

import std;
import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.store.tcp;
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

const char* stateLabel(api::TcpState state) {
    switch (state) {
        case api::TcpState::Disconnected: return "未连接";
        case api::TcpState::Resolving: return "解析中";
        case api::TcpState::Connecting: return "连接中";
        case api::TcpState::Handshaking: return "TLS 握手中";
        case api::TcpState::Connected: return "已连接";
        case api::TcpState::Failed: return "连接失败";
    }
    return "未知";
}

std::string hex(const std::vector<std::uint8_t>& bytes) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) out += ' ';
        out += digits[bytes[i] >> 4U];
        out += digits[bytes[i] & 0x0FU];
    }
    return out;
}

std::string textPreview(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::uint8_t byte : bytes) {
        if (byte == '\n') out += "\\n";
        else if (byte == '\r') out += "\\r";
        else if (byte == '\t') out += "\\t";
        else if (byte >= 0x20U && byte < 0x7FU) out += static_cast<char>(byte);
        else out += '.';
    }
    return out;
}

std::string decodeHex(const std::string& input, std::vector<std::uint8_t>& bytes) {
    std::string compact;
    compact.reserve(input.size());
    for (const unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (!std::isxdigit(c)) return "十六进制只能包含 0-9、A-F 和空白";
        compact += static_cast<char>(c);
    }
    if (compact.empty()) return "消息不能为空";
    if (compact.size() % 2 != 0) return "十六进制字符数必须为偶数";
    const auto value = [](char c) -> std::uint8_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
        return static_cast<std::uint8_t>(c - 'A' + 10);
    };
    bytes.clear();
    bytes.reserve(compact.size() / 2);
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>((value(compact[i]) << 4U) | value(compact[i + 1])));
    }
    return {};
}

std::string encodeMessage(const RequestTab& tab, std::vector<std::uint8_t>& bytes) {
    if (tab.tcpSendFormat == api::TcpPayloadFormat::Hex) return decodeHex(tab.tcpMessage, bytes);
    if (tab.tcpMessage.empty()) return "消息不能为空";
    bytes.assign(tab.tcpMessage.begin(), tab.tcpMessage.end());
    return {};
}

void saveTcpRequest() {
    RequestTab& tab = activeTab();
    db::SavedRequest request{
        .id = tab.requestId,
        .groupId = tab.draft.groupId,
        .name = tab.draft.name,
        .kind = tab.draft.kind,
        .method = "GET",
        .url = tab.draft.url,
    };
    const std::string error = g_requests.save(request);
    if (!error.empty()) { showStatus("保存失败: " + error); return; }
    tab.requestId = request.id;
    showStatus("已保存: " + request.name);
}

} // namespace

export void drawTcpPage(eui::Ui& ui, float x, float y, float w, float h,
                        const AppTheme& theme) {
    RequestTab& tab = activeTab();
    Draft& draft = tab.draft;
    const auto& tokens = theme.components;
    const std::string id = "tcp." + std::to_string(tab.uid);
    const api::TcpState state = g_tcp.state(tab.uid);
    tab.tcpState = state;
    const bool busy = state == api::TcpState::Resolving || state == api::TcpState::Connecting ||
                      state == api::TcpState::Handshaking || state == api::TcpState::Connected;
    const bool tls = trim(draft.url).starts_with("tcps://");

    constexpr float connectW = 84.0f;
    constexpr float saveW = 52.0f;
    constexpr float timeoutW = 64.0f;
    const float urlW = std::max(120.0f, w - connectW - saveW - timeoutW - kGap * 3.0f);
    components::input(ui, id + ".url")
        .position(x, y).size(urlW, kInputHeight).value(draft.url)
        .placeholder(tls ? "tcps://example.com:443" : "tcp://127.0.0.1:9000")
        .fontFamily("monospace").theme(tokens)
        .onChange([](const std::string& value) { activeDraft().url = value; }).build();
    components::input(ui, id + ".timeout")
        .position(x + urlW + kGap, y).size(timeoutW, kInputHeight)
        .value(std::to_string(draft.tcpConnectTimeoutSec)).placeholder("超时")
        .theme(tokens).onChange([](const std::string& value) {
            try { activeDraft().tcpConnectTimeoutSec = std::clamp(std::stoi(value), 1, 120); }
            catch (...) {}
        }).build();
    components::button(ui, id + ".connect")
        .position(x + urlW + timeoutW + kGap * 2.0f, y).size(connectW, kButtonHeight)
        .icon(busy ? 0xF04D : 0xF1D8).text(busy ? "断开" : "连接")
        .fontSize(kFontLabel).theme(tokens, true)
        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
        .onClick([busy] {
            RequestTab& active = activeTab();
            if (busy) { g_tcp.disconnect(active.uid); return; }
            const std::string error = g_tcp.connect(active.uid, api::TcpSpec{
                .url = trim(active.draft.url),
                .connectTimeoutSec = active.draft.tcpConnectTimeoutSec,
            });
            showStatus(error.empty() ? "TCP 连接中…" : error);
        }).build();
    components::button(ui, id + ".save")
        .position(x + urlW + timeoutW + connectW + kGap * 3.0f, y).size(saveW, kButtonHeight)
        .icon(0xF0C7).text("保存").fontSize(kFontLabel).theme(tokens, false)
        .onClick([] { saveTcpRequest(); }).build();

    ui.text(id + ".state")
        .position(x, y + 32.0f).size(w, 18.0f)
        .text(std::string(tls ? "TCPS · " : "TCP · ") + stateLabel(state) + " · " + trim(draft.url))
        .fontSize(kFontLabel).lineHeight(18.0f)
        .color(state == api::TcpState::Failed ? theme.serverErr : theme.metaText)
        .verticalAlign(core::VerticalAlign::Center).build();

    const float composerH = 58.0f;
    const float timelineY = y + 56.0f;
    const float timelineH = std::max(70.0f, h - 56.0f - composerH - kGap);
    ui.rect(id + ".timeline.panel").position(x, timelineY).size(w, timelineH)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.35f : 0.60f))
        .radius(kPanelRadius).build();
    components::scrollView(ui, id + ".timeline.scroll")
        .position(x + 4.0f, timelineY + 4.0f).size(w - 8.0f, timelineH - 8.0f)
        .offset(tab.tcpScroll).theme(tokens)
        .onChange([](float value) { activeTab().tcpScroll = value; })
        .content([&](eui::Ui& content, float contentWidth, float) {
            if (tab.tcpEvents.empty()) {
                content.stack(id + ".timeline.empty.row").size(contentWidth, 32.0f).content([&] {
                    content.text(id + ".timeline.empty").position(8.0f, 8.0f)
                        .size(contentWidth - 16.0f, 18.0f).text("连接后将在此显示原始 TCP 数据")
                        .fontSize(kFontLabel).color(theme.hintText).build();
                }).build();
            }
            for (int i = 0; i < static_cast<int>(tab.tcpEvents.size()); ++i) {
                const auto& event = tab.tcpEvents[i];
                const std::string row = id + ".timeline." + std::to_string(i);
                std::string prefix;
                switch (event.kind) {
                    case api::TcpEventKind::Connecting: prefix = "CONNECT"; break;
                    case api::TcpEventKind::Connected: prefix = "OPEN"; break;
                    case api::TcpEventKind::Sent: prefix = "→ TX"; break;
                    case api::TcpEventKind::Received: prefix = "← RX"; break;
                    case api::TcpEventKind::Disconnected: prefix = "CLOSE"; break;
                    case api::TcpEventKind::Error: prefix = "ERROR"; break;
                }
                const std::string payload = tab.tcpReceiveFormat == api::TcpPayloadFormat::Hex
                    ? hex(event.payload) : textPreview(event.payload);
                const std::string text = prefix + (event.wireBytes ? " [" + std::to_string(event.wireBytes) + " B] " : " ") +
                                         (!payload.empty() ? payload : event.detail);
                content.stack(row).size(contentWidth, 22.0f).content([&] {
                    content.text(row + ".text").position(8.0f, 2.0f)
                        .size(contentWidth - 16.0f, 18.0f).text(text).fontFamily("monospace")
                        .fontSize(kFontMono)
                        .color(event.kind == api::TcpEventKind::Error ? theme.serverErr : theme.bodyText)
                        .wrap(true).build();
                }).build();
            }
        }).build();

    const float composerY = timelineY + timelineH + kGap;
    components::input(ui, id + ".message")
        .position(x, composerY).size(std::max(80.0f, w - 246.0f), composerH)
        .value(tab.tcpMessage).placeholder(tab.tcpSendFormat == api::TcpPayloadFormat::Hex ? "48 65 6C 6C 6F" : "输入要发送的数据")
        .multiline().fontFamily("monospace").theme(tokens)
        .onChange([](const std::string& value) { activeTab().tcpMessage = value; }).build();
    ui.stack(id + ".send.mode.wrap").position(x + w - 240.0f, composerY).size(112.0f, 24.0f)
        .content([&] {
            components::segmented(ui, id + ".send.mode").size(112.0f, 24.0f)
                .items({"文本", "Hex"}).selected(tab.tcpSendFormat == api::TcpPayloadFormat::Hex ? 1 : 0)
                .theme(tokens).style(segmentedStyle(theme))
                .onChange([](int index) { activeTab().tcpSendFormat = index == 1 ? api::TcpPayloadFormat::Hex : api::TcpPayloadFormat::Text; }).build();
        }).build();
    ui.stack(id + ".receive.mode.wrap").position(x + w - 120.0f, composerY).size(120.0f, 24.0f)
        .content([&] {
            components::segmented(ui, id + ".receive.mode").size(120.0f, 24.0f)
                .items({"接收文本", "接收 Hex"}).selected(tab.tcpReceiveFormat == api::TcpPayloadFormat::Hex ? 1 : 0)
                .theme(tokens).style(segmentedStyle(theme))
                .onChange([](int index) { activeTab().tcpReceiveFormat = index == 1 ? api::TcpPayloadFormat::Hex : api::TcpPayloadFormat::Text; }).build();
        }).build();
    components::button(ui, id + ".send")
        .position(x + w - 120.0f, composerY + 32.0f).size(120.0f, 26.0f)
        .icon(0xF1D8).text("发送数据").fontSize(kFontLabel).theme(tokens, true)
        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
        .onClick([] {
            RequestTab& active = activeTab();
            std::vector<std::uint8_t> bytes;
            if (const std::string error = encodeMessage(active, bytes); !error.empty()) { showStatus(error); return; }
            if (const std::string error = g_tcp.send(active.uid, bytes); !error.empty()) { showStatus(error); return; }
            active.tcpMessage.clear();
        }).build();
}
