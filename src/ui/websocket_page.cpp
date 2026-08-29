// websocket_page.cpp — WebSocket 调试：连接/断开 + 文本/二进制发送 + 事件流。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.api_engine;
import apitab.store.websocket;

namespace apitab::ui {

namespace {
constexpr std::int64_t kUid = 1; // HuxerUI 版单工作区，固定会话 id

std::string StateName(api::WebSocketState s) {
    switch (s) {
        case api::WebSocketState::Connected: return "已连接";
        case api::WebSocketState::Connecting: return "连接中";
        case api::WebSocketState::Failed: return "失败";
        default: return "未连接";
    }
}
} // namespace

// 事件流：独立重组作用域 —— 每 150ms 的 events 更新只重绘事件区。
[[huxerui::composable]] huxerui::View WsEventStream(huxerui::State<std::vector<std::string>> events,
                                                  const huxerui::ThemeSpec& theme) {
    return huxerui::ScrollView{huxerui::Column {
        huxerui::ForEach(events.Get(), [theme](const std::string& line) {
            return huxerui::Text(line, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant));
        }),
    }
                               .With(huxerui::Frame{.height = 300.0F})};
}

[[huxerui::composable]] huxerui::View WebSocketPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    auto message = huxerui::UseState(huxerui::TextEditingValue{});
    auto binary = huxerui::UseState(false);
    auto connected = huxerui::UseState(false);
    auto status = huxerui::UseState(std::string{"未连接"});
    auto events = huxerui::UseState<std::vector<std::string>>({});

    // 事件泵：页面存活期间持续 drain 引擎事件。
    huxerui::Lifecycle(
        [=] -> void {
            tasks.Launch([=]() -> huxerui::Task<void> {
                while (true) {
                    std::vector<std::string> lines = events.Get();
                    for (const api::WebSocketEvent& e : g_websocket.drain(kUid)) {
                        switch (e.kind) {
                            case api::WebSocketEventKind::Open:
                                lines.push_back("● 已连接");
                                connected = true;
                                status = "已连接";
                                break;
                            case api::WebSocketEventKind::Text:
                                lines.push_back("← " + e.payload);
                                break;
                            case api::WebSocketEventKind::Binary:
                                lines.push_back("← [binary " + std::to_string(e.wireBytes) + "B]");
                                break;
                            case api::WebSocketEventKind::Close:
                                lines.push_back("○ 连接关闭" +
                                                (e.closeCode ? " (code " + std::to_string(e.closeCode) + ")" : ""));
                                connected = false;
                                status = "未连接";
                                break;
                            case api::WebSocketEventKind::Error:
                                lines.push_back("✗ " + e.detail);
                                connected = false;
                                status = "失败";
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

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("WebSocket", "状态: " + status.Get()),
        huxerui::TextField(url)
            .Label("ws:// 或 wss:// 地址")
            .Placeholder("wss://echo.example.com")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; }),
        huxerui::Row {
            huxerui::Button("连接").OnClick([=] {
                api::WebSocketSpec spec;
                spec.url = url.Get().text;
                if (const std::string err = g_websocket.connect(kUid, spec); !err.empty())
                    toast.Show(err);
            }),
            huxerui::Button("断开").OnClick([=] {
                g_websocket.disconnect(kUid);
                connected = false;
                status = "未连接";
            }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Row {
            huxerui::Switch("二进制", binary),
            huxerui::TextField(message)
                .Label("消息")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([message](const huxerui::TextEditingValue& value) { message = value; }),
            huxerui::Button("发送").OnClick([=] {
                if (const std::string err =
                        g_websocket.send(kUid, message.Get().text, binary.Get());
                    !err.empty())
                    toast.Show(err);
                else
                    events = [&] {
                        std::vector<std::string> lines = events.Get();
                        lines.push_back((binary.Get() ? "→ [binary] " : "→ ") + message.Get().text);
                        return lines;
                    }();
            }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Text("事件", huxerui::TextRole::Title),
        WsEventStream(events, theme),
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))};
}

} // namespace apitab::ui
