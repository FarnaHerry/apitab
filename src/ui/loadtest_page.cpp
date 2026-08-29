// loadtest_page.cpp — k6 压测：参数输入 + 启动/停止 + 实时输出流 + 汇总与最近记录。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.api_engine;
import apitab.db;
import apitab.preferences;
import apitab.store.loadtest;
import apitab.store.requests;

namespace apitab::ui {

namespace {
constexpr std::array<std::string_view, 7> kMethodNames{
    "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
constexpr std::size_t kOutputCap = 300;
} // namespace

// 输出流：独立重组作用域 —— 压测期间每 200ms 的 output 更新只重绘输出区。
[[huxerui::composable]] huxerui::View OutputArea(huxerui::State<std::vector<std::string>> output,
                                                 const huxerui::ThemeSpec& theme) {
    return huxerui::ScrollView{huxerui::Column {
        huxerui::ForEach(output.Get(), [theme](const std::string& line) {
            return huxerui::Text(line, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant));
        }),
    }
                               .With(huxerui::Frame{.height = 260.0F})}.With(huxerui::ScrollBar());
}

// 汇总行：独立重组作用域。
[[huxerui::composable]] huxerui::View SummaryLine(huxerui::State<std::string> summary) {
    return huxerui::Text(summary.Get(), huxerui::TextRole::Body);
}

[[huxerui::composable]] huxerui::View LoadTestPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    auto methodIndex = huxerui::UseState<std::size_t>(0);
    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    auto vus = huxerui::UseState(huxerui::TextEditingValue{"10"});
    auto duration = huxerui::UseState(huxerui::TextEditingValue{"30s"});
    auto running = huxerui::UseState(false);
    auto output = huxerui::UseState<std::vector<std::string>>({});
    auto summary = huxerui::UseState<std::string>("");

    const bool k6ok = g_loadtest.available();

    // 岛屿分区模型：本页只有一个岛，岛本身占满整个页面区块（Grow + Stretch），
    // 内容在岛内部滚动——不再用「ScrollView 套自包含岛」的倒装结构。
    return huxerui::Column {
        PageHeader("压测", k6ok ? "k6 引擎：就绪" : "k6 引擎：未找到（engines/ 或 PATH）"),
        huxerui::ScrollView{huxerui::Column {
            huxerui::Row {
                DropdownSelect(
                    std::vector<std::string>(kMethodNames.begin(), kMethodNames.end()),
                    methodIndex.Get(),
                    [methodIndex](std::size_t index) { methodIndex = index; }),
                huxerui::TextField(url)
                    .Label("目标 URL")
                    .Placeholder("https://api.example.com/v1/resource")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; })
                    .With(huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::Row {
                huxerui::TextField(vus)
                    .Label("VUs")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([vus](const huxerui::TextEditingValue& value) { vus = value; }),
                huxerui::TextField(duration)
                    .Label("时长")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .OnChanged([duration](const huxerui::TextEditingValue& value) { duration = value; }),
            }
                .With(huxerui::Spacing(theme.spacing.medium)),
            huxerui::Row {
                huxerui::Button(running.Get() ? "压测进行中…" : "开始压测")
                    .OnClick([=] {
                        if (running.Get()) return;
                        api::RequestSpec spec;
                        spec.method = std::string{kMethodNames.at(methodIndex.Get())};
                        spec.url = url.Get().text;
                        if (spec.url.empty()) {
                            toast.Show("URL 不能为空");
                            return;
                        }
                        api::LoadOptions opts;
                        opts.vus = std::atoi(vus.Get().text.c_str());
                        if (opts.vus <= 0) opts.vus = 1;
                        opts.duration = duration.Get().text.empty() ? "30s" : duration.Get().text;
                        output = std::vector<std::string>{};
                        summary = "";
                        running = true;
                        g_loadtest.start(spec, opts, 0, "loadtest");
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            std::vector<std::string> lines;
                            api::LoadSummary s;
                            while (g_loadtest.running() || !g_loadtest.pollSummary(s)) {
                                for (std::string& line : g_loadtest.drainOutput()) {
                                    lines.push_back(std::move(line));
                                    if (lines.size() > kOutputCap)
                                        lines.erase(lines.begin(), lines.begin() + (lines.size() - kOutputCap));
                                }
                                output = lines;
                                co_await huxerui::Delay(std::chrono::duration<double>{0.2});
                            }
                            g_loadtest.pollSummary(s);
                            if (s.ok) {
                                summary = std::format("{} 请求 · RPS {:.0f} · P50 {:.0f}ms · P95 {:.0f}ms "
                                                      "· P99 {:.0f}ms · 失败率 {:.2f}%",
                                                      s.requests, s.rps, s.p50Ms, s.p95Ms, s.p99Ms,
                                                      s.failRate * 100.0);
                            } else {
                                summary = "压测异常结束: " + s.error;
                            }
                            running = false;
                        });
                    }),
                huxerui::Button("停止")
                    .OnClick([=] {
                        g_loadtest.stop();
                        toast.Show("已发送停止信号");
                    }),
            }
                .With(huxerui::Spacing(theme.spacing.medium)),
            SummaryLine(summary),
            huxerui::Text("输出", huxerui::TextRole::Title),
            // 输出区自带固定高度滚动（同轴嵌套滚动是有意的：输出流独立滚动）。
            OutputArea(output, theme),
            huxerui::Text("最近记录", huxerui::TextRole::Title),
            huxerui::ForEach(
                g_loadtest.records(10), [theme](const db::LoadRecord& r) {
                    return huxerui::Text(
                               std::format("{} {} · VU={} · {} req · RPS {:.0f} · P95 {:.0f}ms", r.name,
                                           r.url, r.vus, r.requests, r.rps, r.p95Ms),
                               huxerui::TextRole::Body)
                        .With(huxerui::Foreground(theme.colors.on_surface_variant));
                }),
        }
                            .With(huxerui::Spacing(theme.spacing.medium),
                                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
