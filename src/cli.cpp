// cli.cpp — 命令行模式实现（`apitab --cli <子命令> …`，无 GUI）。
//
// 与 GUI 共享的关键事实：
//   - 同一个领域单例 RequestStore g_requests（构造即打开 SQLite
//     <数据目录>/apitab.db，并持有 curl 引擎 = 常驻工作线程），CLI 进程与 GUI
//     进程互不通信：SQLite 按连接加锁，两进程同时读写靠锁串行；store 的
//     guarded() 把偶发 SQLITE_BUSY 转成错误消息字符串返回，不抛穿到 main。
//   - 项目上下文默认从 settings.ini 的 session.active_project 恢复（与 GUI
//     启动时一致）；`--project` 显式覆盖时经 selectProjectInOrg 切换并**回写
//     active_project**（下次 GUI 启动会打开到该项目的标签）；`--org` 与
//     send/show 内部跨项目自动定位只做进程内内存切换，不回写。
//   - `--env` 只影响本进程的 selectEnv（finalizeSpec 的 {{变量}} 替换与
//     baseUrl 拼接都读它），不持久化。
//   - send 成功后经 recordHistory 落历史（与 GUI 发送一致），供 `history`
//     命令和 GUI 历史页查看；Mock 启用的条目照抄 GUI 语义：不走真实网络、
//     不落历史。
//
// 线程契约：GUI 里引擎结果由 UI 协程 PollWhile 轮询 takeResponse；CLI 无事件
// 循环，主线程 sleep(10ms) 轮询（引擎工作线程与结果槽契约见 curl_engine.cpp
// 注释：send 纯入队、结果槽内部加锁，跨线程取用安全）。120s 墙钟兜底后
// cancelSend() 协作打断；退出时 CurlEngine 析构自带 cancel+join，不会卡死。
//
// 退出码约定：0 成功；1 用法/数据错误（参数非法、ID 不存在、URL 为空等）；
// 2 请求传输失败（引擎 ok=false 或超时；HTTP 状态码 >=400 不算失败，状态在
// 输出里）。stdout 只放数据（列表一行一条 / show 块 / send 结果），错误与
// 提示一律 stderr。
#include "cli.h"

// localtime_r/localtime_s 是平台 C 扩展，不经 import std 暴露；按仓库约定
// 放在 import std 之前（CLAUDE.md 关键约定 1）。
#include <ctime>

import std;
import nlohmann.json;
import apitab.api_engine;
import apitab.db;
import apitab.utils;
import apitab.preferences;
import apitab.store.requests;

namespace apitab::cli {
namespace {

// ---- 小工具 ----------------------------------------------------------------

void out(std::string_view line) { std::cout << line << '\n'; }
void err(std::string_view line) { std::cerr << line << '\n'; }

std::optional<std::int64_t> parseI64(std::string_view s) {
    std::int64_t v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

const char* bodyKindName(api::BodyKind kind) {
    switch (kind) {
        case api::BodyKind::None: return "none";
        case api::BodyKind::Json: return "json";
        case api::BodyKind::Text: return "text";
        case api::BodyKind::FormUrlEncoded: return "x-www-form-urlencoded";
        case api::BodyKind::FormData: return "form-data";
        case api::BodyKind::Xml: return "xml";
        case api::BodyKind::GraphQL: return "graphql";
    }
    return "none";
}

bool isFormKind(api::BodyKind kind) {
    return kind == api::BodyKind::FormUrlEncoded || kind == api::BodyKind::FormData;
}

const char* requestKindName(api::RequestKind kind) {
    switch (kind) {
        case api::RequestKind::Http: return "HTTP";
        case api::RequestKind::WebSocket: return "WebSocket";
        case api::RequestKind::Tcp: return "TCP";
    }
    return "?";
}

std::string kvOnOff(bool enabled) { return enabled ? "on" : "off"; }

// 当前时区的 "YYYY-MM-DD HH:MM"（formatTime 只到 MM-DD，历史/时间戳要年份）。
std::string stampTime(std::int64_t unixSec) {
    const std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", tm.tm_year + 1900,
                       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ---- 参数解析 ----------------------------------------------------------------

// 各子命令支持的选项互不相同；不允许的选项报「未知选项」。
struct Options {
    std::vector<std::string> positional;
    std::int64_t orgId = -1;      // -1 = 未指定
    std::int64_t projectId = -1;  // -1 = 未指定
    std::string env;              // --env 原文（数字=ID，否则=环境名）
    std::int64_t limit = -1;      // --limit（-1 = 未指定）
    bool json = false;
    bool help = false;
    std::string error;            // 非空 = 解析失败（调用方打 stderr 退出 1）
};

// 只解析该子命令允许的选项；不允许的选项/缺值/非数字 ID 记入 opt.error。
Options parseOptions(const std::vector<std::string>& args, bool allowOrg,
                     bool allowProject, bool allowEnv, bool allowLimit,
                     bool allowJson) {
    Options opt;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--help" || a == "-h") {
            opt.help = true;
            continue;
        }
        if (a == "--json") {
            if (!allowJson) { opt.error = "该子命令不支持 --json"; return opt; }
            opt.json = true;
            continue;
        }
        if (a == "--org" || a == "--project" || a == "--env" || a == "--limit") {
            const bool org = a == "--org";
            const bool project = a == "--project";
            const bool env = a == "--env";
            const bool limit = a == "--limit";
            const bool permitted = (org && allowOrg) || (project && allowProject) ||
                                   (env && allowEnv) || (limit && allowLimit);
            if (!permitted) { opt.error = "该子命令不支持 " + a; return opt; }
            if (i + 1 >= args.size()) { opt.error = a + " 缺少取值"; return opt; }
            const std::string& value = args[++i];
            if (env) {
                opt.env = value;
                continue;
            }
            const std::optional<std::int64_t> id = parseI64(value);
            if (!id || *id < 0) { opt.error = a + " 需要非负整数"; return opt; }
            if (org) opt.orgId = *id;
            else if (project) opt.projectId = *id;
            else opt.limit = *id;
            continue;
        }
        if (a.starts_with("--")) { opt.error = "未知选项: " + a; return opt; }
        opt.positional.push_back(a);
    }
    return opt;
}

// ---- 项目上下文 --------------------------------------------------------------

// 恢复/切换项目上下文。返回退出码（1 = 数据错误，已打 stderr）。
// 规则（与 skill 文档一致）：
//   --project：跨组织反查所属 org 后 selectProjectInOrg，并回写 active_project；
//   --org：selectOrg；原活动项目若不属于该组织则自动打开其第一个项目（不回写）；
//   都不给：从 session active_project 恢复（不回写）。
int ensureContext(const Options& opt) {
    if (opt.projectId >= 0) {
        std::int64_t orgId = 0;
        bool found = false;
        for (const db::Project& p : g_requests.allProjects()) {
            if (p.id == opt.projectId) { orgId = p.orgId; found = true; break; }
        }
        if (!found) { err("项目不存在: " + std::to_string(opt.projectId)); return 1; }
        if (opt.orgId >= 0 && opt.orgId != orgId) {
            err("--project 与 --org 不属于同一组织"); return 1;
        }
        if (const std::string e = g_requests.selectProjectInOrg(orgId, opt.projectId);
            !e.empty()) {
            err("切换项目失败: " + e); return 1;
        }
        saveSessionPreference("active_project", std::to_string(opt.projectId));
        return 0;
    }
    if (opt.orgId >= 0) {
        bool found = false;
        for (const db::Org& o : g_requests.orgs()) {
            if (o.id == opt.orgId) { found = true; break; }
        }
        if (!found) { err("组织不存在: " + std::to_string(opt.orgId)); return 1; }
        if (const std::string e = g_requests.selectOrg(opt.orgId); !e.empty()) {
            err("切换组织失败: " + e); return 1;
        }
        if (g_requests.currentProjectId() == 0 && !g_requests.projects().empty()) {
            (void)g_requests.selectProject(g_requests.projects().front().id);
        }
        return 0;
    }
    // 默认：恢复会话活动项目（不回写偏好）。
    const std::string active = trim(sessionPreference("active_project"));
    if (const std::optional<std::int64_t> id = parseI64(active); id && *id > 0) {
        for (const db::Project& p : g_requests.allProjects()) {
            if (p.id != *id) continue;
            (void)g_requests.selectProjectInOrg(p.orgId, p.id);
            break;
        }
    }
    return 0;
}

// 在当前项目找请求；找不到则跨项目自动定位（临时切上下文、不回写偏好）。
// 返回副本——切项目会重载 store 缓存，不能把 find() 的裸指针带出本函数。
std::optional<db::SavedRequest> locateRequest(std::int64_t id) {
    if (const db::SavedRequest* r = g_requests.find(id)) return *r;
    const std::int64_t keep = g_requests.currentProjectId();
    for (const db::Project& p : g_requests.allProjects()) {
        if (p.id == keep) continue;
        if (!g_requests.selectProjectInOrg(p.orgId, p.id).empty()) continue;
        if (const db::SavedRequest* r = g_requests.find(id)) return *r;
    }
    return std::nullopt;
}

// ---- help -------------------------------------------------------------------

void printGeneralHelp() {
    out(R"(用法: apitab --cli <子命令> [参数]

子命令（全部支持 --help 查看单条详情）:
  help                              本帮助
  orgs                              列出组织（* = 当前）
  projects [--org ID]               列出项目（默认当前组织的）
  requests [--org ID] [--project ID]        列出项目内请求
  show <请求ID> [--project ID]                查看单请求全字段
  send <请求ID> [--project ID] [--env ID|名字] [--json]
                                    发送请求并输出响应（body 到 stdout）
  history [--limit N]               最近发送历史（默认 20 条）

约定:
  stdout 数据（列表一行一条）；stderr 错误。
  退出码 0=成功 1=用法/数据错误 2=请求传输失败（HTTP >=400 不算失败）。
  与 GUI 共用 ~/.local/share/apitab（SQLite + settings.ini）；--project
  覆盖会回写 active_project，影响下次 GUI 启动的活动项目。)");
}

void printOrgsHelp() {
    out("用法: apitab --cli orgs\n\n列出全部组织，* 标注当前组织。");
}

void printProjectsHelp() {
    out(R"(用法: apitab --cli projects [--org ID]

列出组织（默认当前）下的项目，* 标注当前活动项目。
--org ID 切换组织上下文（不回写会话偏好）。)");
}

void printRequestsHelp() {
    out(R"(用法: apitab --cli requests [--org ID] [--project ID]

列出目标项目的请求：ID / 方法 / 名称 / URL / 分组 / 更新时间。
不给 --project 时用会话活动项目；输出首行标注当前 项目/环境 上下文。)");
}

void printShowHelp() {
    out(R"(用法: apitab --cli show <请求ID> [--project ID]

打印单请求全字段：params / headers / cookies / body（含表单字段）/
测试用例 / Mock 配置。请求不在当前项目时自动跨项目定位。)");
}

void printSendHelp() {
    out(R"(用法: apitab --cli send <请求ID> [--project ID] [--env ID|名字] [--json]

组装请求（SpecFromSaved）→ finalizeSpec（环境变量替换 + baseUrl 拼接 +
合并全局 Cookie/公共头 + 全局超时/代理）→ curl 引擎发送 → 10ms 轮询取回，
墙钟 120s 兜底超时。成功后落历史（与 GUI 发送一致）。

  --env     发送前切换环境（数字=ID，0=无环境；否则按环境名精确匹配）
  --json    输出结构化 JSON（单行）：ok/method/url/status/durationMs/
            sizeBytes/headers/body/error，便于脚本与 AI 解析
  非 HTTP 条目与 Mock 启用条目的语义与 GUI 一致（Mock 不走网络、不落历史）。

退出码: 0 成功（含 HTTP >=400）；1 用法/数据错误；2 传输失败或超时。)");
}

void printHistoryHelp() {
    out(R"(用法: apitab --cli history [--limit N]

最近发送历史（全局列表，最新在前，默认 20 条）：
ID / 时间 / 方法 / 状态 / 耗时 / 大小 / URL /（错误）/ 关联请求。)");
}

// ---- orgs / projects / requests ----------------------------------------------

int cmdOrgs(const Options& opt) {
    if (opt.help) { printOrgsHelp(); return 0; }
    if (!opt.positional.empty()) { err("orgs 不接受参数（见 apitab --cli help）"); return 1; }
    for (const db::Org& o : g_requests.orgs()) {
        out(std::format("{} {}\t{}", o.id == g_requests.currentOrgId() ? "*" : " ",
                        o.id, o.name));
    }
    return 0;
}

int cmdProjects(const Options& opt) {
    if (opt.help) { printProjectsHelp(); return 0; }
    if (!opt.positional.empty()) { err("projects 不接受位置参数（见 --help）"); return 1; }
    if (const int rc = ensureContext(opt); rc != 0) return rc;
    for (const db::Project& p : g_requests.projects()) {
        out(std::format("{} {}\torg={} {}\t(公共头 {})",
                        p.id == g_requests.currentProjectId() ? "*" : " ", p.id, p.orgId,
                        p.name, p.headers.size()));
    }
    return 0;
}

int cmdRequests(const Options& opt) {
    if (opt.help) { printRequestsHelp(); return 0; }
    if (!opt.positional.empty()) { err("requests 不接受位置参数（见 --help）"); return 1; }
    if (const int rc = ensureContext(opt); rc != 0) return rc;
    if (g_requests.currentProjectId() == 0) {
        err("未选择项目：GUI 里打开过项目，或用 --project <ID> 指定（先跑 apitab --cli projects 查 ID）");
        return 1;
    }
    std::string envDesc = "无";
    if (const db::Environment* e = g_requests.findEnvironment(g_requests.currentEnvId())) {
        envDesc = std::format("{} {} ({})", e->id, e->name, e->baseUrl);
    }
    out(std::format("# 项目 {} · 环境 {}", g_requests.currentProjectId(), envDesc));
    for (const db::SavedRequest& r : g_requests.list()) {
        std::string group;
        if (r.groupId != 0) {
            if (const db::Group* g = g_requests.findGroup(r.groupId)) group = " · 分组 " + g->name;
        }
        out(std::format("{}\t{}\t{}\t{}\t(更新 {}{})", r.id, r.method, r.name, r.url,
                        formatTime(r.updatedAt), group));
    }
    return 0;
}

// ---- show --------------------------------------------------------------------

void printKvList(std::string_view label, const std::vector<api::KeyValue>& rows) {
    out(std::format("{} ({}):", label, rows.size()));
    for (const api::KeyValue& kv : rows) {
        std::string line =
            std::format("  [{}] {} = {}", kvOnOff(kv.enabled), kv.key, kv.value);
        if (!kv.type.empty() || !kv.remark.empty()) {
            line += std::format("   (type={} 备注={})", kv.type, kv.remark);
        }
        out(line);
    }
}

int cmdShow(const Options& opt) {
    if (opt.help) { printShowHelp(); return 0; }
    if (opt.positional.size() != 1) { err("show 需要一个请求 ID（见 --help）"); return 1; }
    const std::optional<std::int64_t> id = parseI64(opt.positional[0]);
    if (!id || *id <= 0) { err("请求 ID 需要是正整数"); return 1; }
    if (const int rc = ensureContext(opt); rc != 0) return rc;
    const std::optional<db::SavedRequest> found = locateRequest(*id);
    if (!found) { err("请求不存在: " + std::to_string(*id)); return 1; }
    const db::SavedRequest& r = *found;

    out(std::format("请求 #{}", r.id));
    out("名称: " + r.name);
    std::string group = "未分组";
    if (r.groupId != 0) {
        if (const db::Group* g = g_requests.findGroup(r.groupId)) {
            group = std::format("{} (id {} · {})", g->name, g->id,
                                db::groupModeName(g->mode));
        }
    }
    out(std::format("类型: {}   项目: {}   分组: {}", requestKindName(r.kind),
                    r.projectId, group));
    if (r.kind == api::RequestKind::WebSocket) out("WS 子协议: " + r.wsProtocol);
    out("方法: " + r.method);
    out("URL: " + r.url);
    printKvList("Params", r.params);
    printKvList("Headers", r.headers);
    printKvList("Cookies", r.cookies);
    out(std::format("Body: {}   跟随重定向: {}   JSON 注释剥离: {}",
                    bodyKindName(r.bodyKind), r.followRedirects ? "是" : "否",
                    r.allowJsonComments ? "是" : "否"));
    const std::size_t kindIdx = static_cast<std::size_t>(r.bodyKind);
    const api::BodyContent* content =
        kindIdx < r.bodyContents.size() ? &r.bodyContents[kindIdx] : nullptr;
    if (content && isFormKind(r.bodyKind)) {
        printKvList("表单字段", content->fields);
    } else if (content && !content->text.empty()) {
        out("Body 内容:");
        std::cout << content->text;
        if (!content->text.ends_with('\n')) std::cout << '\n';
    } else if (!r.body.empty()) {
        out("Body 内容:");
        std::cout << r.body;
        if (!r.body.ends_with('\n')) std::cout << '\n';
    }
    out(std::format("测试用例 ({}):", r.testCases.size()));
    for (const db::RequestTestCase& c : r.testCases) {
        out(std::format("  [{}] {}   期望状态={}   最大耗时={}   JSON 断言 {} 条",
                        kvOnOff(c.enabled), c.name.empty() ? "(未命名)" : c.name,
                        c.expectStatus == 0 ? "不校验" : std::to_string(c.expectStatus),
                        c.maxMs > 0.0 ? formatMs(c.maxMs) : "不校验",
                        c.asserts.size()));
        for (const db::RequestAssertion& a : c.asserts) {
            out(std::format("      [{}] {} == {}", kvOnOff(a.enabled), a.path, a.equals));
        }
    }
    out(std::format("Mock: {}", r.mock.enabled ? "启用" : "未启用"));
    if (r.mock.enabled) {
        out(std::format("  状态码={}   延迟={}ms", r.mock.status, r.mock.delayMs));
        printKvList("  Mock Headers", r.mock.headers);
        if (!r.mock.body.empty()) {
            out("  Mock Body:");
            std::cout << r.mock.body;
            if (!r.mock.body.ends_with('\n')) std::cout << '\n';
        }
    }
    out("更新时间: " + stampTime(r.updatedAt));
    return 0;
}

// ---- send --------------------------------------------------------------------

// SavedRequest → RequestSpec：与 GUI 的 SpecFromDraft 等价的纯数据版
// （只取 enabled 且 key 非空的行；form 用结构化字段、文本体用当前类型槽位，
// 槽位空则回落 legacy body 字段——对齐 DraftFromSaved）。
api::RequestSpec specFromSaved(const db::SavedRequest& saved) {
    api::RequestSpec spec;
    spec.method = saved.method;
    spec.url = saved.url;
    for (const api::KeyValue& kv : saved.params)
        if (kv.enabled && !kv.key.empty()) spec.params.push_back(kv);
    for (const api::KeyValue& kv : saved.headers)
        if (kv.enabled && !kv.key.empty()) spec.headers.push_back(kv);
    for (const api::KeyValue& kv : saved.cookies)
        if (kv.enabled && !kv.key.empty()) spec.cookies.push_back(kv);
    spec.bodyKind = saved.bodyKind;
    spec.followRedirects = saved.followRedirects;
    spec.allowJsonComments = saved.allowJsonComments;
    const std::size_t kindIdx = static_cast<std::size_t>(saved.bodyKind);
    if (kindIdx < saved.bodyContents.size()) {
        const api::BodyContent& content = saved.bodyContents[kindIdx];
        if (isFormKind(saved.bodyKind)) {
            for (const api::KeyValue& kv : content.fields)
                if (kv.enabled && !kv.key.empty()) spec.bodyFields.push_back(kv);
        } else {
            spec.body = content.text;
        }
    }
    if (spec.body.empty() && !isFormKind(saved.bodyKind) &&
        saved.bodyKind != api::BodyKind::None) {
        spec.body = saved.body;  // 旧数据只有 legacy body
    }
    return spec;
}

// --env ID|名字 → selectEnv。返回退出码。
int applyEnvOption(const std::string& envArg) {
    if (envArg.empty()) return 0;
    if (const std::optional<std::int64_t> id = parseI64(envArg); id && *id >= 0) {
        if (const std::string e = g_requests.selectEnv(*id); !e.empty()) {
            err("选择环境失败: " + e); return 1;
        }
        return 0;
    }
    const db::Environment* match = nullptr;
    int matches = 0;
    for (const db::Environment& e : g_requests.environments()) {
        if (e.name == envArg) { match = &e; ++matches; }
    }
    if (matches == 0) { err("环境不存在: " + envArg + "（用 requests 命令查看环境列表）"); return 1; }
    if (matches > 1) { err("环境名不唯一: " + envArg); return 1; }
    if (const std::string e = g_requests.selectEnv(match->id); !e.empty()) {
        err("选择环境失败: " + e); return 1;
    }
    return 0;
}

nlohmann::json responseJson(const std::string& method, const std::string& url,
                            const api::ResponseView& view, bool mock) {
    nlohmann::json j;
    j["ok"] = view.ok;
    j["error"] = view.error;
    j["mock"] = mock;
    j["method"] = method;
    j["url"] = url;
    j["status"] = view.status;
    j["durationMs"] = view.totalMs;
    j["sizeBytes"] = view.sizeBytes;
    j["headers"] = nlohmann::json::array();
    for (const api::KeyValue& h : view.headers)
        j["headers"].push_back({{"key", h.key}, {"value", h.value}});
    j["body"] = view.body;
    return j;
}

// 把 mock 定义合成为一个 ResponseView（与 request_page.cpp 的 Mock 拦截分支同口径）。
api::ResponseView mockResponse(const db::SavedRequest& saved) {
    api::ResponseView view;
    view.ok = true;
    view.status = saved.mock.status;
    view.totalMs = static_cast<double>(std::max(0, saved.mock.delayMs));
    view.sizeBytes = static_cast<std::int64_t>(saved.mock.body.size());
    view.body = saved.mock.body;
    for (const api::KeyValue& h : saved.mock.headers) {
        if (!h.enabled || h.key.empty()) continue;
        view.headers.push_back(api::KeyValue{.key = h.key, .value = h.value});
    }
    return view;
}

int cmdSend(const Options& opt) {
    if (opt.help) { printSendHelp(); return 0; }
    if (opt.positional.size() != 1) { err("send 需要一个请求 ID（见 --help）"); return 1; }
    const std::optional<std::int64_t> id = parseI64(opt.positional[0]);
    if (!id || *id <= 0) { err("请求 ID 需要是正整数"); return 1; }
    if (const int rc = ensureContext(opt); rc != 0) return rc;
    const std::optional<db::SavedRequest> found = locateRequest(*id);
    if (!found) { err("请求不存在: " + std::to_string(*id)); return 1; }
    const db::SavedRequest& saved = *found;
    if (saved.kind != api::RequestKind::Http) {
        err(std::format("条目 #{} 是 {} 请求，CLI send 仅支持 HTTP", saved.id,
                        requestKindName(saved.kind)));
        return 1;
    }
    if (const int rc = applyEnvOption(opt.env); rc != 0) return rc;

    // Mock 拦截：照抄 GUI——不发真实请求、不落历史、按定义直接"返回"。
    if (saved.mock.enabled) {
        if (saved.mock.delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(saved.mock.delayMs));
        }
        const api::ResponseView view = mockResponse(saved);
        if (opt.json) {
            out(responseJson(saved.method, saved.url, view, /*mock=*/true).dump());
        } else {
            out(std::format("MOCK HTTP {} · {} · {}", view.status,
                            formatMs(view.totalMs), formatBytes(view.sizeBytes)));
            if (!view.body.empty()) {
                std::cout << view.body;
                if (!view.body.ends_with('\n')) std::cout << '\n';
            }
        }
        return 0;
    }

    api::RequestSpec spec = specFromSaved(saved);
    if (trim(spec.url).empty()) { err("URL 为空，无法发送"); return 1; }
    // finalizeSpec：当前环境的 {{变量}} 替换 + baseUrl/全局 Cookie/公共头合并 +
    // 全局超时/代理注入（与 GUI 发送前同一步）。
    const api::RequestSpec final = g_requests.finalizeSpec(spec);
    if (trim(final.url).empty()) { err("URL 为空（相对路径需当前环境配置 baseUrl，或用 --env 选择环境）"); return 1; }

    g_requests.sendViaEngine(final);
    // 无 UI 事件循环：10ms 节拍轮询引擎结果槽；120s 墙钟兜底（curl 自身有
    // 全局超时，兜底只为引擎线程异常挂死时仍能退出）。
    api::ResponseView view;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    bool got = false;
    while (!got) {
        got = g_requests.takeResponse(view);
        if (got) break;
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!got) {
        g_requests.cancelSend();  // 协作打断在途传输，别让引擎线程被甩在析构后
        err("发送超时（>120s 未取回结果），已请求取消");
        return 2;
    }
    // 落历史（与 GUI 一致；写失败内部吞掉，不打断输出）。
    g_requests.recordHistory(saved.id, final.method, final.url, view);
    if (!view.ok) {
        if (opt.json) {
            out(responseJson(final.method, final.url, view, /*mock=*/false).dump());
        }
        err("请求失败: " + view.error);
        return 2;
    }
    if (opt.json) {
        // error_handler_t::replace：二进制响应体可能含非法 UTF-8，dump 默认会抛。
        out(responseJson(final.method, final.url, view, /*mock=*/false)
                .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
        return 0;
    }
    out(std::format("HTTP {} · {} {} · {} · {}", view.status, final.method, final.url,
                    formatMs(view.totalMs), formatBytes(view.sizeBytes)));
    if (!view.body.empty()) {
        std::cout << view.body;
        if (!view.body.ends_with('\n')) std::cout << '\n';
    }
    return 0;
}

// ---- history -------------------------------------------------------------------

int cmdHistory(const Options& opt) {
    if (opt.help) { printHistoryHelp(); return 0; }
    if (!opt.positional.empty()) { err("history 不接受位置参数（见 --help）"); return 1; }
    int limit = 20;
    if (opt.limit >= 0) {
        if (opt.limit == 0 || opt.limit > 10000) { err("--limit 需要在 1..10000"); return 1; }
        limit = static_cast<int>(opt.limit);
    }
    const std::vector<db::HistoryEntry> rows = g_requests.history(limit);
    out(std::format("# 历史共 {} 条，显示最近 {} 条（最新在前）", g_requests.historyCount(),
                    rows.size()));
    for (const db::HistoryEntry& h : rows) {
        std::string line = std::format("#{}\t{}\t{}\t{}\t状态 {} · {} · {}", h.id,
                                       stampTime(h.createdAt), h.method, h.url, h.status,
                                       formatMs(h.durationMs), formatBytes(h.sizeBytes));
        if (!h.error.empty()) line += " · 失败: " + h.error;
        if (h.requestId != 0) line += std::format("   (请求 #{})", h.requestId);
        out(line);
    }
    return 0;
}

} // namespace

int run(const std::vector<std::string>& args) {
    if (args.empty()) {
        printGeneralHelp();
        err("缺少子命令（用法如上；单条详情 apitab --cli <子命令> --help）");
        return 1;
    }
    const std::string cmd = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());

    if (cmd == "help" || cmd == "--help" || cmd == "-h") {
        printGeneralHelp();
        return 0;
    }

    // help 不碰数据库；其余命令需要 store 健康（构造失败=目录不可写等，见
    // RequestStore 构造函数的 healthy_ 兜底）。
    if (!g_requests.healthy()) {
        err("无法打开 apitab 数据库（检查数据目录可写：~/.local/share/apitab）");
        return 1;
    }

    // 选项解析错误先于 --help 短路（help 显式给出时仍让各命令打印帮助并退 0）。
    auto dispatch = [](Options opt, int (*fn)(const Options&)) -> int {
        if (!opt.error.empty() && !opt.help) {
            err(opt.error + "（该子命令 --help 查看详情）");
            return 1;
        }
        return fn(opt);
    };
    if (cmd == "orgs") return dispatch(parseOptions(rest, false, false, false, false, false), cmdOrgs);
    if (cmd == "projects") return dispatch(parseOptions(rest, true, false, false, false, false), cmdProjects);
    if (cmd == "requests") return dispatch(parseOptions(rest, true, true, false, false, false), cmdRequests);
    if (cmd == "show") return dispatch(parseOptions(rest, false, true, false, false, false), cmdShow);
    if (cmd == "send") return dispatch(parseOptions(rest, false, true, true, false, true), cmdSend);
    if (cmd == "history") return dispatch(parseOptions(rest, false, false, false, true, false), cmdHistory);

    err("未知子命令: " + cmd + "（apitab --cli help 查看全部）");
    return 1;
}

} // namespace apitab::cli
