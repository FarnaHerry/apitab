// tcp_page.cpp — 原始 TCP 调试：连接/断开 + 文本/Hex 发送 + 事件流。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.api_engine;
import apitab.store.tcp;

namespace apitab::ui {

namespace {
constexpr std::int64_t kUid = 1; // HuxerUI 版单工作区，固定会话 id

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<std::uint8_t> FromHex(const std::string& text) {
    std::vector<std::uint8_t> bytes;
    int high = -1;
    for (const char c : text) {
        const int v = HexNibble(c);
        if (v < 0) continue;
        if (high < 0) {
            high = v;
        } else {
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | v));
            high = -1;
        }
    }
    return bytes;
}

std::string StateName(api::TcpState s) {
    switch (s) {
        case api::TcpState::Connected: return "已连接";
        case api::TcpState::Resolving: return "解析中";
        case api::TcpState::Connecting: return "连接中";
        case api::TcpState::Handshaking: return "TLS 握手中";
        case api::TcpState::Failed: return "失败";
        default: return "未连接";
    }
}
} // namespace

// 事件流：独立重组作用域 —— 每 150ms 的 events 更新只重绘事件区。
// 不定高：由调用方用 Grow 分配剩余高度，本区内部滚动。
[[huxerui::composable]] huxerui::View TcpEventStream(huxerui::State<std::vector<std::string>> events,
                                                  const huxerui::ThemeSpec& theme) {
    return huxerui::ScrollView{huxerui::Column {
        huxerui::ForEach(events.Get(), [theme](const std::string& line) {
            return huxerui::Text(line, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant));
        }),
    }
                               .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
        .With(huxerui::ScrollBar());
}

[[huxerui::composable]] huxerui::View TcpPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    auto address = huxerui::UseState(huxerui::TextEditingValue{});
    auto message = huxerui::UseState(huxerui::TextEditingValue{});
    auto hex = huxerui::UseState(false);
    auto status = huxerui::UseState(std::string{"未连接"});
    auto events = huxerui::UseState<std::vector<std::string>>({});

    huxerui::Lifecycle(
        [=] -> void {
            tasks.Launch([=]() -> huxerui::Task<void> {
                while (true) {
                    std::vector<std::string> lines = events.Get();
                    for (const api::TcpEvent& e : g_tcp.drain(kUid)) {
                        switch (e.kind) {
                            case api::TcpEventKind::Connected:
                                lines.push_back("● 已连接");
                                status = "已连接";
                                break;
                            case api::TcpEventKind::Received: {
                                std::string preview(reinterpret_cast<const char*>(e.payload.data()),
                                                    std::min<std::size_t>(e.payload.size(), 200));
                                lines.push_back("← " + preview);
                                break;
                            }
                            case api::TcpEventKind::Disconnected:
                                lines.push_back("○ 连接关闭");
                                status = "未连接";
                                break;
                            case api::TcpEventKind::Error:
                                lines.push_back("✗ " + e.detail);
                                status = "失败";
                                break;
                            default:
                                break;
                        }
                    }
                    if (lines.size() > 300)
                        lines.erase(lines.begin(), lines.begin() + (lines.size() - 300));
                    events = lines;
                    co_await huxerui::Delay(std::chrono::duration<double>{0.15});
                }
            });
        },
        0);

    // 本页嵌在请求页右岛里（右岛已有 Padding/Background）：根 Column 占满右岛
    // 剩余区块（Grow + Stretch），操作区在顶部，事件流 Grow 吃满剩余高度并内部
    // 滚动——不再整页套 ScrollView 自包含收缩。
    return huxerui::Column {
        PageHeader("TCP", "状态: " + status.Get()),
        huxerui::TextField(address)
            .Label("tcp:// 或 tcps:// 地址")
            .Placeholder("tcp://127.0.0.1:9000")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([address](const huxerui::TextEditingValue& value) { address = value; }),
        huxerui::Row {
            huxerui::Button("连接").OnClick([=] {
                api::TcpSpec spec;
                spec.url = address.Get().text;
                if (const std::string err = g_tcp.connect(kUid, spec); !err.empty())
                    toast.Show(err);
            }),
            huxerui::Button("断开").OnClick([=] {
                g_tcp.disconnect(kUid);
                status = "未连接";
            }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Row {
            huxerui::Switch("Hex 发送", hex),
            huxerui::TextField(message)
                .Label(hex.Get() ? "Hex 字节（如 68 65 6C 6C 6F）" : "文本消息")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([message](const huxerui::TextEditingValue& value) { message = value; })
                .With(huxerui::Grow(1.0F)),
            huxerui::Button("发送").OnClick([=] {
                const std::vector<std::uint8_t> bytes =
                    hex.Get() ? FromHex(message.Get().text)
                              : std::vector<std::uint8_t>(message.Get().text.begin(),
                                                          message.Get().text.end());
                if (bytes.empty()) {
                    toast.Show("发送内容为空");
                    return;
                }
                if (const std::string err = g_tcp.send(kUid, bytes); !err.empty())
                    toast.Show(err);
            }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Text("事件", huxerui::TextRole::Title),
        TcpEventStream(events, theme).With(huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.medium), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
