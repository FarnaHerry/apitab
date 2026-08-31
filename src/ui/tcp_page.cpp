// tcp_page.cpp — 原始 TCP 调试：连接/断开 + 文本/Hex 发送 + 事件流。
// 会话由本页 TaskScope 协程直接持有：整条生命周期（连接 → 读循环 → 关闭）在
// 一个协程里，阻塞 IO 经 RunOnTaskThread 上任务线程，恢复后在 UI 线程写 State。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"
#include "task_bridge.h"
#include "tcp_session.h"

import apitab.api_engine;

namespace apitab::ui {

namespace {

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
} // namespace

// 事件流：独立重组作用域 —— 会话协程的 events 更新只重绘事件区。
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
    // 当前会话：空 = 未连接。页面卸载时 State 释放，会话析构即关闭连接。
    auto session = huxerui::UseState(std::shared_ptr<TcpSession>{});

    // 追加一行事件（带 300 行裁剪）。UI 线程调用。
    auto appendEvent = [events](std::string line) {
        std::vector<std::string> lines = events.Get();
        lines.push_back(std::move(line));
        if (lines.size() > 300)
            lines.erase(lines.begin(), lines.begin() + (lines.size() - 300));
        events = lines;
    };

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
                auto s = std::make_shared<TcpSession>();
                session = s; // 取代旧会话（旧会话析构自动关闭）
                status = "连接中";
                tasks.Launch([=]() -> huxerui::Task<void> {
                    // 整条会话生命周期都在这个协程里；页面卸载时 TaskScope 取消
                    // 本协程，配合会话析构的 close() 打断阻塞中的 read。
                    std::string err =
                        co_await RunOnTaskThread([s, spec] { return s->connect(spec); });
                    if (session.Get() != s) co_return; // 已被取代/断开
                    if (!err.empty()) {
                        appendEvent("✗ " + err);
                        status = "失败";
                        session = std::shared_ptr<TcpSession>{};
                        co_return;
                    }
                    status = "已连接";
                    appendEvent("● 已连接");
                    for (;;) {
                        api::TcpEvent e =
                            co_await RunOnTaskThread([s] { return s->read(); });
                        if (session.Get() != s) co_return; // 已被取代/断开
                        if (e.kind == api::TcpEventKind::Received) {
                            std::string preview(
                                reinterpret_cast<const char*>(e.payload.data()),
                                std::min<std::size_t>(e.payload.size(), 200));
                            appendEvent("← " + preview);
                            continue;
                        }
                        if (e.kind == api::TcpEventKind::Disconnected) {
                            appendEvent("○ 连接关闭");
                            status = "未连接";
                        } else {
                            appendEvent("✗ " + e.detail);
                            status = "失败";
                        }
                        session = std::shared_ptr<TcpSession>{};
                        co_return;
                    }
                });
            }),
            huxerui::Button("断开").OnClick([=] {
                if (const auto& s = session.Get()) s->close(); // 唤醒阻塞 read，协程收尾
                session = std::shared_ptr<TcpSession>{};
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
                const auto& s = session.Get();
                if (!s || !s->isConnected()) {
                    toast.Show("TCP 尚未连接");
                    return;
                }
                const std::vector<std::uint8_t> bytes =
                    hex.Get() ? FromHex(message.Get().text)
                              : std::vector<std::uint8_t>(message.Get().text.begin(),
                                                          message.Get().text.end());
                if (bytes.empty()) {
                    toast.Show("发送内容为空");
                    return;
                }
                // 同步写可能阻塞（对端不收）：派任务线程，不卡 UI。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    std::string err = co_await RunOnTaskThread([s, bytes] { return s->send(bytes); });
                    if (!err.empty()) toast.Show(err);
                });
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
