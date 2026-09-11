// websocket_page.cpp — WebSocket 调试：连接/断开 + 文本/二进制发送 + 事件流。
// 会话由本页 TaskScope 直接持有（State<shared_ptr<WsSession>>）：IXWebSocket 自管
// 内部线程，回调事件进会话队列，UI 协程经 PollWhile 泵按节拍 drain 写 State。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <charconv>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "task_bridge.h"
#include "ws_session.h"

import apitab.api_engine;

namespace apitab::ui {

namespace {
std::string HexPreview(std::string_view bytes, std::size_t limit = 128) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string result;
    const std::size_t count = std::min(bytes.size(), limit);
    result.reserve(count * 3 + 4);
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        if (i) result += ' ';
        result += kDigits[byte >> 4];
        result += kDigits[byte & 0x0F];
    }
    if (bytes.size() > count) result += " …";
    return result;
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool DecodeHex(const std::string& text, std::string& bytes, std::string& error) {
    bytes.clear();
    int high = -1;
    for (const char c : text) {
        const int value = HexNibble(c);
        if (value < 0) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ':' || c == '-') continue;
            error = "Hex 内容只能包含十六进制字符及空格、冒号或连字符";
            return false;
        }
        if (high < 0) high = value;
        else {
            bytes += static_cast<char>((high << 4) | value);
            high = -1;
        }
    }
    if (high >= 0) {
        error = "Hex 内容必须由完整的字节组成（缺少一个半字节）";
        return false;
    }
    return true;
}

int PositiveInt(const huxerui::TextEditingValue& value, int fallback) {
    int parsed = fallback;
    const std::string& text = value.text;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && parsed > 0
               ? parsed
               : fallback;
}

std::vector<api::KeyValue> ToHeaders(const huxerui::StateList<KvRow>& rows) {
    std::vector<api::KeyValue> headers;
    for (const KvRow& row : rows) {
        if (!row.key.text.empty())
            headers.push_back({.key = row.key.text, .value = row.value.text, .enabled = row.enabled});
    }
    return headers;
}
} // namespace

// 事件流：独立重组作用域 —— 每 150ms 的 events 更新只重绘事件区。
// 不定高：由调用方用 Grow 分配剩余高度，本区内部滚动。
[[huxerui::composable]] huxerui::View WsEventStream(huxerui::StateList<std::string> events,
                                                  const huxerui::ThemeSpec& theme) {
    return huxerui::VirtualList(
               events, [theme](const std::string& line) {
            return huxerui::Text(line, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant));
        })
        .EstimatedItemExtent(20.0F)
        .CacheExtent(120.0F)
        .With(huxerui::ScrollBar(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View WebSocketPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    auto message = huxerui::UseState(huxerui::TextEditingValue{});
    auto binary = huxerui::UseState(false);
    auto binaryHex = huxerui::UseState(false);
    auto showBinaryHex = huxerui::UseState(true);
    auto connected = huxerui::UseState(false);
    auto status = huxerui::UseState(std::string{"未连接"});
    auto events = huxerui::UseStateList<std::string>();
    // 当前会话：空 = 未连接。页面卸载时 State 释放，会话析构即停 IX 线程。
    auto session = huxerui::UseState(std::shared_ptr<WsSession>{});
    const auto headers = huxerui::UseStateList<KvRow>();
    auto subprotocol = huxerui::UseState(huxerui::TextEditingValue{});
    auto timeout = huxerui::UseState(huxerui::TextEditingValue{"15"});

    // 事件泵：页面存活期间持续从当前会话 drain（IX 线程投递，UI 协程按 150ms
    // 节拍取回并写 State；页面卸载时 TaskScope 取消本协程）。
    huxerui::Lifecycle(
        [=] -> void {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await PollWhile(std::chrono::duration<double>{0.15}, [&] {
                    const auto& s = session.Get();
                    if (!s) return true;
                    std::vector<api::WebSocketEvent> drained = s->drain();
                    if (drained.empty()) return true;
                    for (const api::WebSocketEvent& e : drained) {
                        switch (e.kind) {
                            case api::WebSocketEventKind::Open:
                                events.PushBack("● 已连接");
                                connected = true;
                                status = "已连接";
                                break;
                            case api::WebSocketEventKind::Text:
                                events.PushBack("← " + e.payload);
                                break;
                            case api::WebSocketEventKind::Binary:
                                events.PushBack("← [binary " + std::to_string(e.wireBytes) + "B] " +
                                               (showBinaryHex.Get() ? HexPreview(e.payload) : e.payload));
                                break;
                            case api::WebSocketEventKind::Close:
                                events.PushBack("○ 连接关闭" +
                                               (e.closeCode ? " (code " + std::to_string(e.closeCode) + ")" : ""));
                                connected = false;
                                status = "未连接";
                                break;
                            case api::WebSocketEventKind::Error:
                                events.PushBack("✗ " + e.detail);
                                connected = false;
                                status = "失败";
                                break;
                        }
                    }
                    while (events.Size() > 300) events.Erase(0);
                    return true; // 持续泵到页面卸载
                });
            });
        },
        0);

    // 本页嵌在请求页右岛里（右岛已有 Padding/Background）：根 Column 占满右岛
    // 剩余区块（Grow + Stretch），操作区在顶部，事件流 Grow 吃满剩余高度并内部
    // 滚动——不再整页套 ScrollView 自包含收缩。
    return huxerui::Column {
        PageHeader("WebSocket", "状态: " + status.Get()),
        huxerui::TextField(url)
            .Label("ws:// 或 wss:// 地址")
            .Placeholder("wss://echo.example.com")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; }),
        huxerui::Row {
            huxerui::TextField(subprotocol)
                .Label("子协议（可选）")
                .Placeholder("graphql-ws")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([subprotocol](const huxerui::TextEditingValue& value) { subprotocol = value; })
                .With(huxerui::Grow(1.0F)),
            huxerui::TextField(timeout)
                .Label("握手超时（秒）")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([timeout](const huxerui::TextEditingValue& value) { timeout = value; })
                .With(huxerui::Frame{.width = 130.0F}),
        }.With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Text("握手请求头", huxerui::TextRole::Label),
        KvTableStateList(headers, theme, "名称", "值", [] {},
                         KvTableOptions{.show_type = false, .show_remark = false})
            .With(huxerui::Grow(1.0F)),
        huxerui::Row {
            huxerui::Button("连接").OnClick([=] {
                api::WebSocketSpec spec;
                spec.url = url.Get().text;
                spec.headers = ToHeaders(headers);
                spec.subprotocol = subprotocol.Get().text;
                spec.handshakeTimeoutSec = PositiveInt(timeout.Get(), 15);
                auto s = std::make_shared<WsSession>();
                if (const std::string err = s->connect(spec); !err.empty()) {
                    toast.Show(err);
                    return;
                }
                session = s; // 取代旧会话（旧会话析构自动断开）
            }),
            huxerui::Button("断开").OnClick([=] {
                if (const auto& s = session.Get()) s->disconnect();
                session = std::shared_ptr<WsSession>{};
                connected = false;
                status = "未连接";
            }),
            huxerui::Button("Ping").OnClick([=] {
                const auto& s = session.Get();
                if (!s) { toast.Show("WebSocket 尚未连接"); return; }
                if (const std::string err = s->ping(); !err.empty()) toast.Show(err);
                else {
                    events.PushBack("→ Ping");
                    while (events.Size() > 300) events.Erase(0);
                }
            }),
            huxerui::Button("清空事件").OnClick([events] { events.Clear(); }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Row {
            huxerui::Switch("二进制", binary)
                .OnChanged([binary](bool checked) { binary = checked; }),
            binary.Get()
                ? huxerui::View{huxerui::Switch("发送 Hex", binaryHex)
                                    .OnChanged([binaryHex](bool checked) { binaryHex = checked; })}
                : huxerui::View{},
            huxerui::Switch("二进制显示为 Hex", showBinaryHex)
                .OnChanged([showBinaryHex](bool checked) { showBinaryHex = checked; }),
            huxerui::TextField(message)
                .Label(binary.Get() && binaryHex.Get() ? "Hex 字节（如 68 65 6C 6C 6F）" : "消息")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([message](const huxerui::TextEditingValue& value) { message = value; })
                .With(huxerui::Grow(1.0F)),
            huxerui::Button("发送").OnClick([=] {
                const auto& s = session.Get();
                if (!s) {
                    toast.Show("WebSocket 尚未连接");
                    return;
                }
                std::string payload = message.Get().text;
                std::string parseError;
                if (binary.Get() && binaryHex.Get()) {
                    std::string decoded;
                    if (!DecodeHex(payload, decoded, parseError)) {
                        toast.Show(parseError);
                        return;
                    }
                    payload = std::move(decoded);
                }
                if (const std::string err = s->send(payload, binary.Get());
                    !err.empty())
                    toast.Show(err);
                else
                    events.PushBack(binary.Get()
                                        ? "→ [binary " + std::to_string(payload.size()) + "B] " +
                                              (binaryHex.Get() ? HexPreview(payload) : payload)
                                        : "→ " + payload);
                while (events.Size() > 300) events.Erase(0);
            }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Text("事件", huxerui::TextRole::Title),
        WsEventStream(events, theme).With(huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.medium), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
