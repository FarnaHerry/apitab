// loadtest_page.cpp — k6 压测：参数输入 + 启动/停止 + 实时输出流 + 汇总。
// 压测记录的归档查看在历史记录页左栏「压测记录」分类。
#include <huxerui/huxerui.h>
#include <huxerui/codeeditor.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "app_resources.h"
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

// ---- 左侧用例树（请求 → 接口用例）-------------------------------------------
// 与请求页左岛同一观感语言（手工 hover/选中底 + Focusable + Button 语义），
// 层级深入一层：顶层 = 集合请求（g_requests 数据，仅 HTTP），其下 = 该请求的
// 压测用例。每个请求**写死一个「接口用例」**（不落库，实时用请求页数据构造），
// 已保存用例来自 automation_tests，另可新建。

// 树行：depth 0 = 请求行，1 = 用例行（缩进 + 前缀点）。选中 = islands.active、
// hover = islands.raised；请求行文字主色、用例行次级色（选中时提亮）。
[[huxerui::composable]] huxerui::View CaseTreeRow(std::string label, int depth, bool selected,
                                                  std::function<void()> onClick) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto hovered = huxerui::UseState(false);
    const bool request_row = depth == 0;
    huxerui::View leading =
        huxerui::Text(request_row ? "▾" : "·", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant));
    return huxerui::Row {
        depth > 0 ? huxerui::View{huxerui::Row{}}.With(huxerui::Frame{.width = 16.0F})
                  : huxerui::View{huxerui::Row{}},
        std::move(leading),
        huxerui::Text(label, huxerui::TextRole::Label)
            .With(huxerui::Grow(1.0F),
                  huxerui::ClipChildren(),
                  huxerui::Foreground(selected || request_row
                                          ? theme.colors.on_surface
                                          : theme.colors.on_surface_variant)),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 7.0F)),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              selected ? huxerui::Background(islands.active)
                       : hovered.Get() ? huxerui::Background(islands.raised)
                                       : huxerui::Background(huxerui::Color::Transparent()),
              huxerui::Indication{}, // 整行反馈 = 手工底色（与设置页分类行同规）
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = label})
        .OnClick(std::move(onClick))
        .On<huxerui::ViewEvents::Hover>(
            [hovered](const huxerui::HoverEvent& e) {
                hovered = e.type == huxerui::HoverEventType::Enter;
            });
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
    // 左树选中：请求 id + 用例 id（0 = 该请求写死的「接口用例」）。caseVersion
    // 只做「新建/删除用例后刷新树」的重组信号。
    auto selectedRequestId = huxerui::UseState<std::int64_t>(0);
    auto selectedCaseId = huxerui::UseState<std::int64_t>(0);
    auto caseVersion = huxerui::UseState(0);
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    const bool k6ok = g_loadtest.available();

    // 当前环境的基础 URL（g_requests 持有环境选择；无环境/为空时该段不渲染）。
    std::string envBaseUrl;
    if (const db::Environment* env = g_requests.findEnvironment(g_requests.currentEnvId()))
        envBaseUrl = env->baseUrl;

    // ---- 用例树数据（组合期快照）----
    // 请求列表 = g_requests 当前项目集合（仅 HTTP）；已保存用例 = automation_tests
    //（按项目缓存）。「接口用例」不落库：树里固定渲染在每请求第一行。
    const auto casesResult = g_loadtest.automationTests();
    const std::vector<db::AutomationTest> loadCases =
        casesResult ? *casesResult : std::vector<db::AutomationTest>{};
    std::vector<db::SavedRequest> httpRequests;
    for (const db::SavedRequest& r : g_requests.list())
        if (r.kind == api::RequestKind::Http) httpRequests.push_back(r);
    // 默认选中：首个请求的「接口用例」（一次组合后不再自动漂移）。
    huxerui::Lifecycle([selectedRequestId] {
        if (selectedRequestId.Get() != 0) return;
        for (const db::SavedRequest& r : g_requests.list())
            if (r.kind == api::RequestKind::Http) {
                selectedRequestId = r.id;
                return;
            }
    });

    // 选中变化 → 预填主区（method/url 来自请求数据；保存用例再带 vus/duration）。
    huxerui::Lifecycle(
        [=] {
            const db::SavedRequest* req = g_requests.find(selectedRequestId.Get());
            if (req == nullptr) return;
            for (std::size_t i = 0; i < kMethodNames.size(); ++i) {
                if (std::string_view{kMethodNames[i]} == req->method) {
                    methodIndex = i;
                    break;
                }
            }
            url = huxerui::TextEditingValue::FromText(req->url);
            if (selectedCaseId.Get() != 0) {
                for (const db::AutomationTest& c : loadCases) {
                    if (c.id != selectedCaseId.Get()) continue;
                    vus = huxerui::TextEditingValue::FromText(std::to_string(c.vus));
                    duration = huxerui::TextEditingValue::FromText(c.duration);
                    break;
                }
            }
        },
        selectedRequestId, selectedCaseId, caseVersion);

    // 新建用例：快照当前选中请求 + 页面当前压测参数，落库并选中新用例。
    const auto createCase = [=] {
        const db::SavedRequest* req = g_requests.find(selectedRequestId.Get());
        if (req == nullptr) {
            toast.Show("先在左侧选择一个请求");
            return;
        }
        db::AutomationTest t = g_loadtest.automationFromRequest(
            *req, std::max(1, std::atoi(vus.Get().text.c_str())),
            duration.Get().text.empty() ? "30s" : duration.Get().text);
        int count = 0;
        for (const db::AutomationTest& c : loadCases)
            if (c.requestId == req->id) ++count;
        t.name = std::format("用例 {}", count + 1);
        if (auto saved = g_loadtest.saveAutomation(t); !saved) {
            toast.Show("新建用例失败: " + saved.error().message);
            return;
        }
        selectedCaseId = t.id;
        caseVersion = caseVersion.Get() + 1;
        toast.Show(std::format("已为「{}」新建 {}", req->name, t.name));
    };

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

    // 岛屿分区模型：岛内左右双栏 —— 左 = 接口用例树（请求 → 用例，层级深入
    // 一层），右 = 主区（参数 + 脚本 + 输出）；内容在右栏内部滚动。
    const IslandTheme islands = ResolveIslandTheme(theme);
    huxerui::View caseTree;
    {
        std::vector<huxerui::View> rows;
        for (const db::SavedRequest& r : httpRequests) {
            const bool requestSelected = selectedRequestId.Get() == r.id;
            rows.push_back(CaseTreeRow(
                std::format("{} · {}", r.method, r.name.empty() ? r.url : r.name), 0,
                requestSelected && selectedCaseId.Get() == 0,
                [selectedRequestId, selectedCaseId, r] {
                    selectedRequestId = r.id;
                    selectedCaseId = 0; // 请求行 = 其写死的「接口用例」
                }));
            for (const db::AutomationTest& c : loadCases) {
                if (c.requestId != r.id) continue;
                rows.push_back(
                    CaseTreeRow(c.name, 1, selectedCaseId.Get() == c.id,
                                [selectedRequestId, selectedCaseId, r, c] {
                                    selectedRequestId = r.id;
                                    selectedCaseId = c.id;
                                })
                        .Key(c.id));
            }
        }
        caseTree = huxerui::ScrollView{huxerui::Column(std::move(rows))
                                           .With(huxerui::Spacing(theme.spacing.extra_small),
                                                 huxerui::CrossAlign(
                                                     huxerui::CrossAxisAlignment::Stretch))}
                       .With(huxerui::ScrollBar(), huxerui::Grow(1.0F));
    }

    // 双面板仿首页（HomePage）：左 = 用例树面板（定宽），右 = 主区岛
    // （PageHeader + 参数/脚本/输出，岛内滚动）。无整页外底板——面板直接
    // 落海面，hairline 线框表达边界；PageHeader 像首页项目岛一样住在岛内。
    return huxerui::Row {
        // 左：用例树面板（请求 → 接口用例）。头部 = 标题 + 新建用例。
            huxerui::Column {
                huxerui::Row {
                    huxerui::Text("接口用例", huxerui::TextRole::Title)
                        .With(huxerui::Grow(1.0F)),
                    AppIconButton(app::images::add, "为选中请求新建用例", createCase,
                                  AppIconButtonShape::RoundedSquare, 28.0F),
                }
                    .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                httpRequests.empty()
                    ? huxerui::View{huxerui::Text("当前项目还没有 HTTP 请求——先在请求页创建",
                                                  huxerui::TextRole::Body)
                                        .With(huxerui::Foreground(
                                            theme.colors.on_surface_variant))}
                    : huxerui::View{std::move(caseTree)},
            }
                .With(huxerui::Padding(theme.spacing.medium),
                      huxerui::Spacing(theme.spacing.small),
                      huxerui::Background(islands.base),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Frame{.width = 250.0F}),
            // 线条风：左右两岛之间只留一条 1px 分隔线（岛无边框）。
            huxerui::Row{}.With(huxerui::Frame{.width = 1.0F},
                                huxerui::Background(islands.outline_hair)),
            // 右：主区岛——内容岛内滚动。页面标题与 k6 引擎状态不再重复展示
            //（k6 就绪与否由底部全局状态栏表达）。
            huxerui::Column {
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
                        // 用例驱动：选中请求/用例时以其实际数据为基底（headers/
                        // body 随行，URL/方法仍取输入框当前值可改）；无选中（项目
                        // 无请求）退回手输模式。
                        api::RequestSpec spec;
                        if (const db::SavedRequest* req =
                                g_requests.find(selectedRequestId.Get())) {
                            if (selectedCaseId.Get() != 0) {
                                bool found = false;
                                for (const db::AutomationTest& c : loadCases) {
                                    if (c.id == selectedCaseId.Get()) {
                                        spec = specFromAutomation(c);
                                        found = true;
                                        break;
                                    }
                                }
                                if (!found)
                                    spec = specFromAutomation(
                                        g_loadtest.automationFromRequest(*req, 0, ""));
                            } else {
                                spec = specFromAutomation(
                                    g_loadtest.automationFromRequest(*req, 0, ""));
                            }
                        }
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
                    .With(huxerui::ScrollBar()),
            }
                .With(huxerui::Padding(compact ? theme.spacing.medium : theme.spacing.large),
                      huxerui::Spacing(theme.spacing.medium),
                      huxerui::Background(ResolveIslandTheme(theme).base),
                      huxerui::Grow(1.0F),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

} // namespace apitab::ui
