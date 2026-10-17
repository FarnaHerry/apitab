// loadtest_page.cpp — k6 压测：参数输入 + 启动/停止 + 实时输出流 + 汇总。
// 压测记录的归档查看在历史记录页左栏「压测记录」分类。
#include <huxerui/huxerui.h>
#include <huxerui/codeeditor.h>

#include <array>
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
#ifdef APITAB_HAS_COMBOBOX
[[huxerui::composable]] huxerui::View DurationField(huxerui::State<huxerui::TextEditingValue> duration) {
    return huxerui::ComboBox(duration.Get(), std::vector<std::string>{"10s", "30s", "1m", "5m"})
        .Label("时长")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([duration](const huxerui::TextEditingValue& value) { duration = value; })
        .OnSelected([duration](std::size_t, const huxerui::TextEditingValue& value) { duration = value; });
}
#else
[[huxerui::composable]] huxerui::View DurationField(huxerui::State<huxerui::TextEditingValue> duration) {
    return huxerui::TextField(duration)
        .Label("时长")
        .Variant(huxerui::TextFieldVariant::Outlined)
        .OnChanged([duration](const huxerui::TextEditingValue& value) { duration = value; });
}
#endif
// kMethodNames 用 draft.h 共享表（curl/k6 都接受任意方法名；框架 HttpClient
// 的 HttpMethod 枚举只有经典 7 个，故 http_test_page 是例外）。
constexpr std::size_t kOutputCap = 300;

// 压测模式名与 SegmentedButton 顺序一一对应。
constexpr std::array<std::string_view, 3> kLoadModeNames{"固定 VUs", "阶梯压测", "固定迭代"};

// 从页面 State 组装 LoadOptions —— 脚本模板生成与「开始压测」共用这一份解析
//（此前 vus/duration 解析在两处重复，阈值等新参数加进来后必须合一）。
api::LoadOptions AssembleOptions(const std::string& vusText, const std::string& durationText,
                                 std::size_t modeIndex, const std::string& rampUpText,
                                 const std::string& peakVusText, const std::string& steadyText,
                                 const std::string& rampDownText, const std::string& iterationsText,
                                 const std::string& p95LimitText, const std::string& failLimitText,
                                 bool skipTls, bool noReuse) {
    api::LoadOptions opts;
    opts.vus = std::atoi(vusText.c_str());
    if (opts.vus <= 0) opts.vus = 1;
    opts.duration = durationText.empty() ? "30s" : durationText;
    opts.mode = modeIndex == 1 ? api::LoadMode::Stages
              : modeIndex == 2 ? api::LoadMode::Iterations
                               : api::LoadMode::Constant;
    opts.rampUpDuration = rampUpText.empty() ? "10s" : rampUpText;
    opts.peakVus = std::atoi(peakVusText.c_str());
    if (opts.peakVus <= 0) opts.peakVus = 1;
    opts.steadyDuration = steadyText.empty() ? "1m" : steadyText;
    opts.rampDownDuration = rampDownText.empty() ? "10s" : rampDownText;
    opts.iterations = std::atoi(iterationsText.c_str());
    if (opts.iterations <= 0) opts.iterations = 1;
    opts.p95LimitMs = std::atof(p95LimitText.c_str());
    opts.failRateLimitPct = std::atof(failLimitText.c_str());
    opts.insecureSkipTlsVerify = skipTls;
    opts.noConnectionReuse = noReuse;
    return opts;
}

// 按当前参数生成 k6 脚本模板（编辑器初始内容 / 「从参数重新生成」共用）。
// 参数取值与「开始压测」的 spec/opts 组装保持一致（含 baseUrl 拼接）。
std::string MakeScriptTemplate(std::size_t methodIndex, const std::string& urlText,
                               const api::LoadOptions& opts) {
    api::RequestSpec spec;
    spec.method = std::string{kMethodNames.at(methodIndex)};
    spec.url = g_requests.composeUrl(urlText, 0, g_requests.currentEnvId());
    return g_loadtest.scriptTemplate(spec, opts);
}

// 状态码分布行："200×1180 · 500×12"；非 2xx 计数用同一文本着色在 UI 侧处理。
std::string StatusCountsText(const std::vector<std::pair<int, std::int64_t>>& counts) {
    std::string out;
    for (const auto& [status, count] : counts) {
        if (!out.empty()) out += " · ";
        out += std::format("{}×{}", status, count);
    }
    return out;
}
} // namespace

// 输出流：独立重组作用域 —— 压测期间每 200ms 的 output 更新只重绘输出区。
[[huxerui::composable]] huxerui::View OutputArea(huxerui::StateList<std::string> output,
                                                 const huxerui::ThemeSpec& theme) {
    return huxerui::VirtualList(
               output, [theme](const std::string& line) {
            return huxerui::Text(line, huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant));
        })
        .EstimatedItemExtent(20.0F)
        .CacheExtent(120.0F)
        .With(huxerui::ScrollBar(), huxerui::Frame{.height = 260.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 汇总区：独立重组作用域。主行 + 阈值判定徽标 + 深度指标（checks / 状态码分布 /
// 数据量 / 迭代时长 / VU 峰值），深度行按是否存在（hasChecks / 非空分布）隐藏。
[[huxerui::composable]] huxerui::View SummaryArea(huxerui::State<api::LoadSummary> summary,
                                                  const huxerui::ThemeSpec& theme) {
    const api::LoadSummary& s = summary.Get();
    if (!s.ok && s.error.empty()) return huxerui::View{};
    std::vector<huxerui::View> lines;
    if (!s.ok) {
        lines.push_back(huxerui::Text("压测异常结束: " + s.error, huxerui::TextRole::Body)
                            .With(huxerui::Foreground(theme.colors.error)));
        return huxerui::Column(std::move(lines)).With(huxerui::Spacing(theme.spacing.small));
    }
    // 主行：核心延迟分位与失败率。
    lines.push_back(huxerui::Text(
        std::format("{} 请求 · RPS {:.0f} · P50 {:.0f}ms · P95 {:.0f}ms · P99 {:.0f}ms"
                    " · 失败率 {:.2f}%",
                    s.requests, s.rps, s.p50Ms, s.p95Ms, s.p99Ms, s.failRate * 100.0),
        huxerui::TextRole::Body));
    // 阈值判定徽标：-1 未启用不显示；0/1 分别染危险/成功色（成功用 on_surface，
    // 主题没有独立的 success token，避免再造语义色）。
    if (s.thresholdOk == 0 || s.thresholdOk == 1) {
        const bool passed = s.thresholdOk == 1;
        lines.push_back(
            huxerui::Text(passed ? "✓ 阈值达标" : "✗ 阈值未达标", huxerui::TextRole::Body)
                .With(huxerui::Foreground(passed ? theme.colors.on_surface
                                                 : theme.colors.error)));
    } else if (s.exitCode == 99) {
        lines.push_back(huxerui::Text("✗ 阈值未达标（k6 退出码 99）", huxerui::TextRole::Body)
                            .With(huxerui::Foreground(theme.colors.error)));
    }
    if (s.hasChecks) {
        lines.push_back(huxerui::Text(
            std::format("checks：{} 通过 / {} 失败", s.checksPassed, s.checksFailed),
            huxerui::TextRole::Body));
    }
    if (!s.statusCounts.empty()) {
        // 旧版 k6 的 status 子指标分布（v2+ 汇总数据不再携带，通常为空）。
        std::vector<huxerui::View> parts;
        for (const auto& [status, count] : s.statusCounts) {
            if (!parts.empty()) parts.push_back(huxerui::Text(" · "));
            parts.push_back(
                huxerui::Text(std::format("{}×{}", status, count), huxerui::TextRole::Body)
                    .With(huxerui::Foreground(status >= 200 && status < 300
                                                  ? theme.colors.on_surface_variant
                                                  : theme.colors.error)));
        }
        parts.insert(parts.begin(),
                     huxerui::Text("状态码 ", huxerui::TextRole::Body)
                         .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        lines.push_back(huxerui::Row(std::move(parts)));
    } else if (s.expectedOkCount >= 0 &&
               s.requests > s.expectedOkCount) {
        // k6 ≥v2：非预期响应（4xx/5xx 与协议错误）= 总请求 - 预期响应计数。
        lines.push_back(
            huxerui::Text(std::format("非预期响应 {} 条", s.requests - s.expectedOkCount),
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.error)));
    }
    std::string extras;
    if (s.dataSentKb > 0 || s.dataReceivedKb > 0)
        extras += std::format("收发 {:.1f} KB / {:.1f} KB", s.dataSentKb, s.dataReceivedKb);
    if (s.iterP50Ms > 0) {
        if (!extras.empty()) extras += " · ";
        extras += std::format("迭代 P50 {:.0f}ms", s.iterP50Ms);
    }
    if (s.vusMax > 0) {
        if (!extras.empty()) extras += " · ";
        extras += std::format("VU 峰值 {}", s.vusMax);
    }
    if (!extras.empty())
        lines.push_back(huxerui::Text(extras, huxerui::TextRole::Body)
                            .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    return huxerui::Column(std::move(lines)).With(huxerui::Spacing(theme.spacing.small));
}

[[huxerui::composable]] huxerui::View LoadTestPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    auto methodIndex = huxerui::UseState<std::size_t>(0);
    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    // 模式：0=固定 VUs 1=阶梯 2=固定迭代（kLoadModeNames 同序）。
    auto modeIndex = huxerui::UseState<std::size_t>(0);
    auto vus = huxerui::UseState(huxerui::TextEditingValue{"10"});
    auto duration = huxerui::UseState(huxerui::TextEditingValue{"30s"});
    // Stages：ramp-up 时长 / 峰值 VU / 稳态时长 / ramp-down 时长。
    auto rampUp = huxerui::UseState(huxerui::TextEditingValue{"10s"});
    auto peakVus = huxerui::UseState(huxerui::TextEditingValue{"50"});
    auto steady = huxerui::UseState(huxerui::TextEditingValue{"1m"});
    auto rampDown = huxerui::UseState(huxerui::TextEditingValue{"10s"});
    // Iterations：总迭代次数。
    auto iterations = huxerui::UseState(huxerui::TextEditingValue{"100"});
    // 阈值（空 = 不启用）。
    auto p95Limit = huxerui::UseState(huxerui::TextEditingValue{});
    auto failLimit = huxerui::UseState(huxerui::TextEditingValue{});
    // HTTP 行为。
    auto skipTls = huxerui::UseState(false);
    auto noReuse = huxerui::UseState(false);
    auto running = huxerui::UseState(false);
    auto output = huxerui::UseStateList<std::string>();
    // 汇总结构直进 State：SummaryArea 按字段富展示（主行/徽标/深度行）。
    auto summary = huxerui::UseState(api::LoadSummary{});
    // k6 脚本编辑器控制器（非受控组件：改文本走 LoadDocument，读文本走 Text()）。
    auto scriptController = huxerui::codeeditor::UseEditorController();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    const bool k6ok = g_loadtest.available();

    // 当前环境的基础 URL（g_requests 持有环境选择；无环境/为空时该段不渲染）。
    std::string envBaseUrl;
    if (const db::Environment* env = g_requests.findEnvironment(g_requests.currentEnvId()))
        envBaseUrl = env->baseUrl;

    // 参数 State → LoadOptions（模板生成与开始压测共用，见 AssembleOptions）。
    const auto assemble = [=] {
        return AssembleOptions(vus.Get().text, duration.Get().text, modeIndex.Get(),
                               rampUp.Get().text, peakVus.Get().text, steady.Get().text,
                               rampDown.Get().text, iterations.Get().text, p95Limit.Get().text,
                               failLimit.Get().text, skipTls.Get(), noReuse.Get());
    };
    const auto makeTemplate = [=] {
        return MakeScriptTemplate(methodIndex.Get(), url.Get().text, assemble());
    };

    // 脚本编辑器初始内容 = 按当前参数生成的模板（非受控：initial_text 只在挂载时
    // 生效，之后参数变了由「从参数重新生成」按钮 LoadDocument 回灌）。
    huxerui::codeeditor::EditorOptions scriptOptions;
    ApplyEditorTypography(scriptOptions);
    ConfigureEditorMenu(scriptOptions);
    scriptOptions.document_key = "k6-script";
    scriptOptions.initial_text = makeTemplate();
    scriptOptions.decoration_providers.push_back(SweetLineProvider(
        std::string{kJavaScriptSyntax}, scriptOptions.initial_text, scriptOptions.document_key));
    scriptOptions.tab_size = 2;  // 与生成的脚本缩进一致
    scriptOptions.theme = EditorTheme(theme);

    // 参数输入的公共属性（Outlined + 受控回写）。
    const auto textField = [](huxerui::State<huxerui::TextEditingValue> state,
                              const char* label) {
        return huxerui::TextField(state)
            .Label(label)
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([state](const huxerui::TextEditingValue& value) { state = value; });
    };

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
            // 压测模式 + 条件参数：Flow 自适应换行（compact 与宽视口同一布局）。
            huxerui::SegmentedButton(std::vector<huxerui::StringVariant>(
                                         kLoadModeNames.begin(), kLoadModeNames.end()),
                                     modeIndex)
                .OnChanged([modeIndex](std::size_t index) { modeIndex = index; }),
            huxerui::Flow {
                modeIndex.Get() == 1
                    ? huxerui::View{huxerui::Flow{textField(rampUp, "爬升"),
                                                  textField(peakVus, "峰值 VUs"),
                                                  textField(steady, "稳态"),
                                                  textField(rampDown, "回落")}}
                : modeIndex.Get() == 2
                    ? huxerui::View{huxerui::Flow{textField(vus, "VUs"),
                                                  textField(iterations, "迭代次数")}}
                    : huxerui::View{huxerui::Flow{textField(vus, "VUs"),
                                                  DurationField(duration)}},
                textField(p95Limit, "P95 上限 ms"),
                textField(failLimit, "失败率上限 %"),
                huxerui::Checkbox("跳过 TLS 校验", skipTls)
                    .OnChanged([skipTls](bool value) { skipTls = value; }),
                huxerui::Checkbox("关闭连接复用", noReuse)
                    .OnChanged([noReuse](bool value) { noReuse = value; }),
            }
                .With(huxerui::Spacing(theme.spacing.small)),
            huxerui::Flow {
                huxerui::Text("脚本", huxerui::TextRole::Title),
                huxerui::Button("从参数重新生成")
                    .OnClick([=] {
                        if (scriptController.LoadDocument("k6-script", makeTemplate()))
                            toast.Show("已按当前参数重新生成脚本");
                    }),
            }
                .With(huxerui::Spacing(theme.spacing.medium)),
            huxerui::codeeditor::CodeEditor(scriptOptions, scriptController)
                .With(huxerui::Frame{.height = 240.0F}),
            huxerui::Flow {
                PrimaryButton(huxerui::Button(running.Get() ? "压测进行中…" : "开始压测")
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
                        // 全局代理对 k6 同样生效（引擎 spawn 时注入子进程环境）。
                        spec.proxy = g_requests.globalProxy();
                        // 参数组装与脚本模板同一份解析（AssembleOptions）。
                        api::LoadOptions opts = assemble();
                        // 编辑器全文作为自定义脚本（编辑器未挂载取到空串时引擎自动
                        // 生成兜底）。
                        opts.script = scriptController.Text();
                        output.Clear();
                        summary = api::LoadSummary{};
                        running = true;
                        g_loadtest.start(spec, opts, 0, "loadtest");
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            // k6 子进程由引擎监视线程拆行入队；UI 协程按 200ms
                            // 节拍取回输出并写 State（见 task_bridge.h 线程契约）。
                            api::LoadSummary s;
                            std::string persistError;
                            co_await PollWhile(std::chrono::duration<double>{0.2}, [&] {
                                for (std::string& line : g_loadtest.drainOutput()) {
                                    output.PushBack(std::move(line));
                                    while (output.Size() > kOutputCap) output.Erase(0);
                                }
                                auto polled = g_loadtest.pollSummary(s);
                                if (!polled) {
                                    persistError = polled.error().message;
                                    return false;
                                }
                                return g_loadtest.running() || !*polled;
                            });
                            if (!persistError.empty()) {
                                s = api::LoadSummary{};
                                s.error = "压测结果落库失败: " + persistError;
                            }
                            // 无论成败都把结构化汇总交给 SummaryArea 展示
                            //（ok=false + error 文案；ok=true 按字段分行）。
                            summary = s;
                            running = false;
                        });
                    })),
                huxerui::Button("停止")
                    .OnClick([=] {
                        g_loadtest.stop();
                        toast.Show("已发送停止信号");
                    }),
            }
                .With(huxerui::Spacing(theme.spacing.medium)),
            SummaryArea(summary, theme),
            huxerui::Text("输出", huxerui::TextRole::Title),
            // 输出区自带固定高度虚拟滚动。
            OutputArea(output, theme),
            // 压测记录的归档查看已迁往历史记录页左栏「压测记录」分类（含阈值
            // 判定染色），本页不再重复列出。
        }
                            .With(huxerui::Spacing(theme.spacing.medium),
                                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
    }
        .With(huxerui::Padding(compact ? theme.spacing.medium : theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.small), huxerui::Grow(1.0F),
              huxerui::Frame{.min_width = 320.0F, .min_height = 240.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
