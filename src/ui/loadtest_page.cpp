// loadtest_page.cpp — k6 压测：参数输入 + 启动/停止 + 实时输出流 + 汇总与最近记录。
#include <huxerui/huxerui.h>
#include <sweetedit_core/sweet_editor.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"
#include "syntax_grammars.h"
#include "task_bridge.h"

import apitab.api_engine;
import apitab.db;
import apitab.preferences;
import apitab.store.loadtest;
import apitab.store.requests;

namespace apitab::ui {

namespace {
// kMethodNames 用 draft.h 共享表（curl/k6 都接受任意方法名；框架 HttpClient
// 的 HttpMethod 枚举只有经典 7 个，故 http_test_page 是例外）。
constexpr std::size_t kOutputCap = 300;

// 按当前参数生成 k6 脚本模板（编辑器初始内容 / 「从参数重新生成」共用）。
// 参数取值与「开始压测」的 spec/opts 组装保持一致（含 baseUrl 拼接）。
std::string MakeScriptTemplate(std::size_t methodIndex, const std::string& urlText,
                               const std::string& vusText, const std::string& durationText) {
    api::RequestSpec spec;
    spec.method = std::string{kMethodNames.at(methodIndex)};
    spec.url = g_requests.composeUrl(urlText, 0, g_requests.currentEnvId());
    api::LoadOptions opts;
    opts.vus = std::atoi(vusText.c_str());
    if (opts.vus <= 0) opts.vus = 1;
    opts.duration = durationText.empty() ? "30s" : durationText;
    return g_loadtest.scriptTemplate(spec, opts);
}
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
    // k6 脚本编辑器控制器（非受控组件：改文本走 LoadDocument，读文本走 Text()）。
    auto scriptController = sweetedit_huxer::UseSweetEditorController();

    const bool k6ok = g_loadtest.available();

    // 当前环境的基础 URL（g_requests 持有环境选择；无环境/为空时该段不渲染）。
    std::string envBaseUrl;
    if (const db::Environment* env = g_requests.findEnvironment(g_requests.currentEnvId()))
        envBaseUrl = env->baseUrl;

    // 脚本编辑器初始内容 = 按当前参数生成的模板（非受控：initial_text 只在挂载时
    // 生效，之后参数变了由「从参数重新生成」按钮 LoadDocument 回灌）。
    sweetedit_huxer::SweetEditorOptions scriptOptions;
    scriptOptions.document_key = "k6-script";
    scriptOptions.syntax_json = std::string{kJavaScriptSyntax};
    scriptOptions.initial_text = MakeScriptTemplate(methodIndex.Get(), url.Get().text,
                                                    vus.Get().text, duration.Get().text);
    scriptOptions.tab_size = 2;  // 与生成的脚本缩进一致
    scriptOptions.palette = EditorPalette(theme); // 本地补丁：配色跟随主题

    // 岛屿分区模型：本页只有一个岛，岛本身占满整个页面区块（Grow + Stretch），
    // 内容在岛内部滚动——不再用「ScrollView 套自包含岛」的倒装结构。
    return huxerui::Column {
        PageHeader("压测", k6ok ? "k6 引擎：就绪" : "k6 引擎：未找到（engines/ 或 PATH）"),
        huxerui::ScrollView{huxerui::Column {
            // 方法+URL 合并控件（自带 Grow 占满整行）。
            MethodUrlBar(
                std::vector<std::string>(kMethodNames.begin(), kMethodNames.end()),
                methodIndex.Get(),
                [methodIndex](std::size_t index) { methodIndex = index; },
                url.Get(),
                [url](const huxerui::TextEditingValue& value) { url = value; },
                envBaseUrl,
                "https://api.example.com/v1/resource"),
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
                huxerui::Text("脚本", huxerui::TextRole::Title),
                huxerui::Button("从参数重新生成")
                    .OnClick([=] {
                        if (scriptController.LoadDocument(
                                "k6-script",
                                MakeScriptTemplate(methodIndex.Get(), url.Get().text,
                                                   vus.Get().text, duration.Get().text),
                                std::string{kJavaScriptSyntax}))
                            toast.Show("已按当前参数重新生成脚本");
                    }),
            }
                .With(huxerui::Spacing(theme.spacing.medium)),
            sweetedit_huxer::SweetEditor(scriptOptions, scriptController)
                .With(huxerui::Frame{.height = 240.0F}),
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
                        // 基础 URL 拼接：输入无 URI scheme 时拼当前环境 baseUrl
                        //（groupId=0：压测页无分组概念；带 scheme 的完整 URL 原样）。
                        spec.url = g_requests.composeUrl(spec.url, 0, g_requests.currentEnvId());
                        api::LoadOptions opts;
                        opts.vus = std::atoi(vus.Get().text.c_str());
                        if (opts.vus <= 0) opts.vus = 1;
                        opts.duration = duration.Get().text.empty() ? "30s" : duration.Get().text;
                        // 编辑器全文作为自定义脚本（编辑器未挂载取到空串时引擎自动
                        // 生成兜底）。
                        opts.script = scriptController.Text();
                        output = std::vector<std::string>{};
                        summary = "";
                        running = true;
                        g_loadtest.start(spec, opts, 0, "loadtest");
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            // k6 子进程由引擎监视线程拆行入队；UI 协程按 200ms
                            // 节拍取回输出并写 State（见 task_bridge.h 线程契约）。
                            std::vector<std::string> lines;
                            api::LoadSummary s;
                            co_await PollWhile(std::chrono::duration<double>{0.2}, [&] {
                                for (std::string& line : g_loadtest.drainOutput()) {
                                    lines.push_back(std::move(line));
                                    if (lines.size() > kOutputCap)
                                        lines.erase(lines.begin(), lines.begin() + (lines.size() - kOutputCap));
                                }
                                output = lines;
                                return g_loadtest.running() || !g_loadtest.pollSummary(s);
                            });
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
