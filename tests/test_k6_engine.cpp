// test_k6_engine.cpp — k6 压测引擎（apitab.k6_engine）的纯逻辑契约测试。
//
// 不拉起真实 k6 进程，只验证不依赖子进程的两条契约：
//   1) BuildScript 脚本生成：options 注入（vus/duration/summaryTrendStats 含 p(99)）、
//      URL/方法/头/正文经 JSON 转义、禁用项与空键过滤、FormUrlEncoded 序列化、
//      K6SUMMARY 汇报协议存在；
//   2) ParseSummaryLine 行解析：合法 K6SUMMARY 行解析出全部指标，前缀不符或
//      JSON 损坏时 ok=false，缺键回落 0。

import std;
import apitab.api_engine;
import apitab.k6_engine;

// 引擎侧唤醒 UI 的钩子（GUI 里由 app.cpp 实现，测试里是空实现）。
namespace core::platform {
void requestUiUpdate() {}
} // namespace core::platform

namespace {

int failures = 0;

void check(bool cond, std::string_view what) {
    if (cond) return;
    std::println("FAIL: {}", what);
    ++failures;
}

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

void testBuildScript() {
    api::RequestSpec spec;
    spec.method = "POST";
    spec.url = "https://example.com/api?x=\"quote\"";
    spec.headers = {{"Content-Type", "application/json", true},
                    {"X-Skip", "no", false},
                    {"", "empty-key", true}};
    spec.bodyKind = api::BodyKind::Json;
    spec.body = "{\"a\":1}";
    api::LoadOptions opts;
    opts.vus = 7;
    opts.duration = "45s";
    opts.timeoutSec = 12;

    const std::string s = api::BuildScript(spec, opts);
    check(s.contains("vus: 7"), "options 注入 vus");
    check(s.contains("duration: \"45s\""), "options 注入 duration");
    check(s.contains("'p(99)'"), "summaryTrendStats 含 p(99)");
    check(s.contains("const kUrl = \"https://example.com/api?x=\\\"quote\\\"\";"), "URL JSON 转义");
    check(s.contains("const kMethod = \"POST\";"), "方法注入");
    check(s.contains("\"Content-Type\": \"application/json\""), "头部注入");
    check(!s.contains("X-Skip"), "禁用头部被过滤");
    check(!s.contains("empty-key"), "空键头部被过滤");
    check(s.contains("const kBody = \"{\\\"a\\\":1}\";"), "JSON 正文转义");
    check(s.contains("timeout: \"12s\""), "单请求超时注入");
    check(s.contains("K6SUMMARY "), "handleSummary 汇报协议存在");

    api::RequestSpec noBody;
    noBody.url = "https://example.com/";
    check(api::BuildScript(noBody, {}).contains("const kBody = null;"), "无正文时 kBody=null");

    // FormUrlEncoded：enabled 过滤 + percent 编码（空格 → +，& → %26）。
    api::RequestSpec form;
    form.url = "https://example.com/form";
    form.bodyKind = api::BodyKind::FormUrlEncoded;
    form.bodyFields = {{"user", "a b", true}, {"token", "x&y", true}, {"off", "z", false}};
    check(api::BuildScript(form, {}).contains("const kBody = \"user=a+b&token=x%26y\";"),
          "FormUrlEncoded 序列化");

    // ---- 模式与深度选项 ----
    check(api::BuildScript(spec, {}).contains("vus: 10, duration: \"30s\""),
          "Constant 模式默认执行器");

    api::LoadOptions stages;
    stages.mode = api::LoadMode::Stages;
    stages.rampUpDuration = "15s";
    stages.peakVus = 80;
    stages.steadyDuration = "2m";
    stages.rampDownDuration = "20s";
    const std::string stagesScript = api::BuildScript(spec, stages);
    check(stagesScript.contains(
              "stages: [{ duration: \"15s\", target: 80 }, "
              "{ duration: \"2m\", target: 80 }, { duration: \"20s\", target: 0 }]"),
          "Stages 模式生成阶梯");
    check(!stagesScript.contains("duration: \"45s\""), "Stages 模式不含 duration 执行器");

    api::LoadOptions iters;
    iters.mode = api::LoadMode::Iterations;
    iters.vus = 5;
    iters.iterations = 250;
    check(api::BuildScript(spec, iters).contains("vus: 5, iterations: 250"),
          "Iterations 模式生成固定迭代");

    api::LoadOptions th;
    th.p95LimitMs = 500;
    th.failRateLimitPct = 2;
    const std::string thScript = api::BuildScript(spec, th);
    check(thScript.contains("http_req_duration: ['p(95)<500']"), "P95 阈值注入");
    check(thScript.contains("http_req_failed: ['rate<0.02']"), "失败率阈值注入（百分比转小数）");

    api::LoadOptions tls;
    tls.insecureSkipTlsVerify = true;
    tls.noConnectionReuse = true;
    const std::string tlsScript = api::BuildScript(spec, tls);
    check(tlsScript.contains("insecureSkipTLSVerify: true"), "跳过 TLS 校验注入");
    check(tlsScript.contains("noConnectionReuse: true"), "关闭连接复用注入");
    check(!api::BuildScript(spec, {}).contains("thresholds:"), "默认不注入阈值");
}

void testParseSummaryLine() {
    const api::LoadSummary s = api::ParseSummaryLine(
        R"(K6SUMMARY {"requests":1200,"rps":39.5,"avg":12.3,"min":1.0,"max":99.0,)"
        R"("p50":10.0,"p90":30.0,"p95":40.0,"p99":55.5,"failRate":0.01})");
    check(s.ok, "合法行 ok=true");
    check(s.requests == 1200, "requests 解析");
    check(near(s.rps, 39.5) && near(s.avgMs, 12.3) && near(s.minMs, 1.0) &&
              near(s.maxMs, 99.0) && near(s.p50Ms, 10.0) && near(s.p90Ms, 30.0) &&
              near(s.p95Ms, 40.0) && near(s.p99Ms, 55.5) && near(s.failRate, 0.01),
          "指标字段解析");

    check(!api::ParseSummaryLine("k6 progress line").ok, "无前缀 ok=false");
    check(!api::ParseSummaryLine("K6SUMMARY {bad json").ok, "JSON 损坏 ok=false");
    check(!api::ParseSummaryLine("").ok, "空行 ok=false");

    const api::LoadSummary partial = api::ParseSummaryLine(R"(K6SUMMARY {"requests":3})");
    check(partial.ok && partial.requests == 3 && near(partial.rps, 0.0) &&
              near(partial.p99Ms, 0.0),
          "缺键回落 0");
    check(!partial.hasChecks && partial.thresholdOk == -1 && partial.statusCounts.empty(),
          "旧式缺省行：无 checks / 阈值未启用 / 无状态码分布");

    // 深度指标：checks、数据量、迭代时长、VU 峰值、阈值判定、状态码分布。
    const api::LoadSummary deep = api::ParseSummaryLine(
        R"(K6SUMMARY {"requests":120,"rps":10,"avg":9,"min":1,"max":50,)"
        R"("p50":8,"p90":20,"p95":30,"p99":45,"failRate":0.05,)"
        R"("checksPassed":115,"checksFailed":5,"dataSent":204800,"dataReceived":409600,)"
        R"("iterP50":105.5,"vusMax":20,"thresholdsOk":0,"expectedOk":100,)"
        R"("statuses":[{"status":200,"count":100},{"status":500,"count":20}]})");
    check(deep.ok && deep.hasChecks && deep.checksPassed == 115 && deep.checksFailed == 5,
          "checks 解析");
    check(near(deep.dataSentKb, 200.0) && near(deep.dataReceivedKb, 400.0), "数据量 KB 换算");
    check(near(deep.iterP50Ms, 105.5) && deep.vusMax == 20, "迭代时长与 VU 峰值解析");
    check(deep.thresholdOk == 0 && deep.expectedOkCount == 100, "阈值判定与预期响应计数解析");
    check(deep.statusCounts.size() == 2 && deep.statusCounts[0].first == 200 &&
              deep.statusCounts[0].second == 100 && deep.statusCounts[1].first == 500 &&
              deep.statusCounts[1].second == 20,
          "状态码分布解析");
}

void testEngineAvailability() {
    const auto none = makeK6Engine("");
    check(!none->available(), "空路径不可用");
    const auto engine = makeK6Engine("/bin/true");
    check(engine->available(), "非空路径可用");
    check(engine->binaryPath() == "/bin/true", "binaryPath 回读");
    check(!engine->running(), "未启动时 running=false");
}

// 可选集成测试（APITAB_TEST_K6 指向真实 k6 二进制才跑，CI 默认跳过）：
// 真实 spawn 验证代理注入——不可达代理 → failRate=1（代理真的进了子进程环境）；
// 空代理 → 直连成功（环境里的代理变量被剥除）。
void testProxyInjection() {
    const char* k6 = std::getenv("APITAB_TEST_K6");
    if (k6 == nullptr || *k6 == '\0') {
        std::println("skip: APITAB_TEST_K6 未设置（代理注入集成测试需要真实 k6 与网络）");
        return;
    }
    auto runOnce = [&](const std::string& proxy) {
        const auto engine = makeK6Engine(k6);
        api::RequestSpec spec;
        spec.url = "https://example.com/";
        spec.proxy = proxy;
        api::LoadOptions opts;
        opts.vus = 1;
        opts.duration = "2s";
        opts.timeoutSec = 5;
        engine->start(spec, opts);
        api::LoadSummary summary;
        while (!engine->takeSummary(summary))
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return summary;
    };
    const api::LoadSummary proxied = runOnce("http://127.0.0.1:1");
    check(proxied.ok, "代理注入集成：拿到汇总（不可达代理）");
    check(near(proxied.failRate, 1.0), "不可达代理 → failRate=1（代理真的生效）");
    const api::LoadSummary direct = runOnce("");
    check(direct.ok, "代理注入集成：拿到汇总（直连）");
    check(near(direct.failRate, 0.0), "空代理 → 直连 failRate=0");
}

} // namespace

int main() {
    testBuildScript();
    testParseSummaryLine();
    testEngineAvailability();
    testProxyInjection();
    if (failures == 0) {
        std::println("k6 engine contract: all checks passed");
        return 0;
    }
    std::println("{} check(s) failed", failures);
    return 1;
}
