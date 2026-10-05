// k6_engine.cpp — apitab.k6_engine 实现单元。
//
// 进程模型（对齐 tinynext aria2 引擎）：start() 在 UI 线程调用，posix_spawn /
// CreateProcess 拉起 k6 子进程（stdout+stderr 合并进一根管道），监视线程流式读
// 输出 —— k6 的进度行用 \r 原地刷新，统一按 \r / \n 拆行入队，节流 150ms
// requestUiUpdate() 唤醒 UI。EOF 后 waitpid / WaitForSingleObject 收尾，
// 从输出里捞脚本 handleSummary 打印的 `K6SUMMARY {json}` 行解析指标。
// stop()：POSIX 先 SIGINT（k6 收到后会优雅收尾并照常打印 summary），监视线程
// 3s 后未退出再 SIGKILL；Windows 没有可靠的跨进程 Ctrl+C，改走 k6 REST API
// PATCH stopped:true 优雅停止（summary 照常产生），API 不可达才退回
// TerminateProcess，监视线程同样 3s 宽限兜底强杀。
// 代理：子进程环境按 spec.proxy 显式注入/剥除代理变量（空 = 直连），与 curl
// 引擎同一契约——k6 只认 HTTP_PROXY/HTTPS_PROXY/NO_PROXY 环境变量。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>   // 必须先于 windows.h
#include <ws2tcpip.h>
#include <windows.h>
#include <stringapiset.h>
#include <wchar.h>      // _wcsicmp（环境块里大小写不敏感地剥代理变量）
#include <curl/curl.h>  // stop() 的 REST PATCH 用（仅 Windows 分支引用）
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>   // errno, EINTR
#include <unistd.h>
extern char** environ;
#endif

// 唤醒 UI 一帧（eui 提供）。前向声明必须放全局模块片段 —— 写在模块域内会被
// 附加模块名修饰（@apitab.k6_engine），链接不到 eui 的符号。
namespace core::platform { void requestUiUpdate(); }

module apitab.k6_engine;

import std;
import nlohmann.json;
import apitab.api_engine;

namespace {

using json = nlohmann::json;

// UI 输出队列里保留的最大行数（压测全程也就几百行进度，防爆内存）。
constexpr std::size_t kMaxOutputLines = 4000;
constexpr auto kWakeThrottle = std::chrono::milliseconds(150);
// stop() 后等 k6 优雅退出的宽限，超时强杀。
constexpr auto kGracePeriod = std::chrono::seconds(3);

// ---- 子进程资源的 RAII 包装 -------------------------------------------------
// k6 子进程与它的输出管道必须由类型持有，禁止在本类里手写 cleanup：
// start() 的每一步都可能失败（posix_spawn 之后的 argv 构造会抛 bad_alloc、
// CreateProcessW 之前的宽字符转换会失败、监视线程创建会抛 system_error），
// 手写 cleanup 只要漏一条分支，管道句柄/进程句柄就漏在引擎的整个生命周期里
// （Windows 上 CreatePipe 的两根句柄尤其容易漏）。析构兜底 = 强杀 + 回收，
// 绝不留僵尸进程。

#ifdef _WIN32
using NativeProcess = HANDLE;
using NativePipe = HANDLE;
constexpr NativeProcess kNoProcess = nullptr;
constexpr NativePipe kNoPipe = nullptr;
#else
using NativeProcess = pid_t;
using NativePipe = int;
constexpr NativeProcess kNoProcess = -1;
constexpr NativePipe kNoPipe = -1;
#endif

// 管道的一端（父进程侧）。析构关闭；release() 把所有权移交出去（装配流程里
// "成功才交给成员"靠它，失败路径由局部 guard 自动收尾）。
class ChildPipe {
public:
    ChildPipe() = default;
    explicit ChildPipe(NativePipe handle) : handle_(handle) {}
    ~ChildPipe() { close(); }

    ChildPipe(const ChildPipe&) = delete;
    ChildPipe& operator=(const ChildPipe&) = delete;

    NativePipe get() const { return handle_; }

    void adopt(NativePipe handle) {
        close();
        handle_ = handle;
    }

    NativePipe release() {
        const NativePipe handle = handle_;
        handle_ = kNoPipe;
        return handle;
    }

    void close() {
        if (handle_ == kNoPipe) return;
#ifdef _WIN32
        ::CloseHandle(handle_);
#else
        ::close(handle_);
#endif
        handle_ = kNoPipe;
    }

private:
    NativePipe handle_ = kNoPipe;
};

// 子进程句柄（Windows: 进程 HANDLE；POSIX: pid）。
// - terminate()：发终止信号（POSIX graceful=SIGINT，否则 SIGKILL；Windows 直接
//   TerminateProcess——没有可靠的跨进程 Ctrl+C）；
// - reap()：阻塞等到退出并回收（POSIX waitpid 收僵尸 / Windows Wait + CloseHandle），
//   幂等，已回收后再调用是空操作；
// - 析构：仍持有就强杀 + 回收，保证任何提前返回/异常路径都不留僵尸。
// 句柄用原子量：terminate() 由 UI 线程调用（stop()），reap() 在监视线程。
class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { abandon(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    void adopt(NativeProcess process) {
        abandon();
        process_.store(process);
    }
    bool running() const { return process_.load() != kNoProcess; }
    // 原生句柄只读访问（监视线程 WaitForSingleObject 轮询退出用），所有权不变。
    NativeProcess nativeHandle() const { return process_.load(); }

    void terminate(bool graceful) {
        const NativeProcess process = process_.load();
        if (process == kNoProcess) return;
#ifdef _WIN32
        (void)graceful;
        ::TerminateProcess(process, 1);
#else
        ::kill(process, graceful ? SIGINT : SIGKILL);
#endif
    }

    void reap() {
        const NativeProcess process = process_.exchange(kNoProcess);
        if (process == kNoProcess) return;
#ifdef _WIN32
        ::WaitForSingleObject(process, INFINITE);
        ::CloseHandle(process);
#else
        int status = 0;
        while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {}
#endif
    }

private:
    void abandon() {
        if (!running()) return;
        terminate(false);
        reap();
    }

    std::atomic<NativeProcess> process_{kNoProcess};
};

#ifndef _WIN32
// posix_spawn 的文件动作表：init 内部持有分配，必须 destroy。它的生命周期跨越
// argv 构造（会抛 bad_alloc），所以同样要 RAII，不能靠 spawn 后手写 destroy。
class SpawnFileActions {
public:
    SpawnFileActions() { ::posix_spawn_file_actions_init(&actions_); }
    ~SpawnFileActions() { ::posix_spawn_file_actions_destroy(&actions_); }

    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;

    posix_spawn_file_actions_t* get() { return &actions_; }

private:
    posix_spawn_file_actions_t actions_;
};
#endif

#ifdef _WIN32
// ---- Windows 优雅停止与代理注入的辅助（RAII）--------------------------------

// Winsock 会话（给 k6 REST API 选回环端口用）。WSAStartup 引用计数，配对 Cleanup。
class WsaSession {
public:
    WsaSession() : ok_(::WSAStartup(MAKEWORD(2, 2), &data_) == 0) {}
    ~WsaSession() { if (ok_) ::WSACleanup(); }

    WsaSession(const WsaSession&) = delete;
    WsaSession& operator=(const WsaSession&) = delete;

    bool ok() const { return ok_; }

private:
    WSADATA data_{};
    bool ok_ = false;
};

class SocketGuard {
public:
    SocketGuard() = default;
    explicit SocketGuard(SOCKET s) : socket_(s) {}
    ~SocketGuard() { if (socket_ != INVALID_SOCKET) ::closesocket(socket_); }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    SOCKET get() const { return socket_; }
    bool valid() const { return socket_ != INVALID_SOCKET; }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

// 给 k6 REST API 选一个空闲回环端口：先 bind 占位拿到端口号再放手（k6 不认
// --address :0，且 bind 失败会以 106 退出、无汇总，不能让它自己撞固定端口）。
// 放手到 k6 bind 之间的竞争窗口极小；真被抢则 k6 退出码 106 → 报异常结束。
int pickFreeLoopbackPort() {
    const WsaSession wsa;
    if (!wsa.ok()) return 0;
    const SocketGuard socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket.valid()) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(socket.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return 0;
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(socket.get(), reinterpret_cast<sockaddr*>(&bound), &len) != 0) return 0;
    return ntohs(bound.sin_port);
}

// GetEnvironmentStringsW 的 RAII 包装（FreeEnvironmentStringsW）。
class EnvironmentStrings {
public:
    EnvironmentStrings() : block_(::GetEnvironmentStringsW()) {}
    ~EnvironmentStrings() { if (block_ != nullptr) ::FreeEnvironmentStringsW(block_); }

    EnvironmentStrings(const EnvironmentStrings&) = delete;
    EnvironmentStrings& operator=(const EnvironmentStrings&) = delete;

    const wchar_t* get() const { return block_; }

private:
    wchar_t* block_ = nullptr;
};

// CreateProcessW 的宽字符环境块（"NAME=VALUE\0...\0\0"）：继承父进程，代理变量
// 按 spec.proxy 处理——空 = 剥掉全部代理变量（直连，不受 http_proxy 影响）；
// 非空 = 注入 HTTP(S)_PROXY 并剥掉 NO_PROXY（Windows 环境名大小写不敏感，
// Go 的 httpproxy 读哪个 case 都等价）。
std::wstring buildEnvironmentBlock(const std::string& proxy) {
    std::wstring block;
    const EnvironmentStrings parent;
    if (parent.get() != nullptr) {
        for (const wchar_t* e = parent.get(); *e != L'\0'; e += std::wcslen(e) + 1) {
            const std::wstring_view entry{e};
            const std::wstring_view name = entry.substr(0, entry.find(L'='));
            bool isProxy = false;
            for (const wchar_t* key : {L"http_proxy", L"https_proxy", L"no_proxy"}) {
                if (name.size() == std::wcslen(key) &&
                    ::_wcsicmp(std::wstring(name).c_str(), key) == 0) {
                    isProxy = true;
                    break;
                }
            }
            if (!isProxy) {
                block += entry;
                block.push_back(L'\0');
            }
        }
    }
    if (!proxy.empty()) {
        const int len = ::MultiByteToWideChar(CP_UTF8, 0, proxy.data(),
                                              static_cast<int>(proxy.size()), nullptr, 0);
        if (len > 0) {
            std::wstring wide(static_cast<std::size_t>(len), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, proxy.data(), static_cast<int>(proxy.size()),
                                  wide.data(), len);
            for (const wchar_t* key : {L"HTTP_PROXY", L"HTTPS_PROXY"}) {
                block += key;
                block.push_back(L'=');
                block += wide;
                block.push_back(L'\0');
            }
        }
    }
    block.push_back(L'\0');
    return block;
}

// 一次性 curl easy 句柄 + 头表（stop() 的 REST PATCH 用）。
// curl 全局初始化的前置条件由 CurlEngine 的 CurlGlobal 满足：GUI 进程先构造
// RequestStore 才可能有压测。curl_easy_init 失败（前置条件不成立）时返回
// 空句柄，调用方退回强杀。
class CurlEasy {
public:
    CurlEasy() : handle_(::curl_easy_init()) {}
    ~CurlEasy() {
        if (headers_ != nullptr) ::curl_slist_free_all(headers_);
        if (handle_ != nullptr) ::curl_easy_cleanup(handle_);
    }

    CurlEasy(const CurlEasy&) = delete;
    CurlEasy& operator=(const CurlEasy&) = delete;

    CURL* get() const { return handle_; }
    curl_slist* headers() const { return headers_; }
    void addHeader(const char* header) {
        headers_ = ::curl_slist_append(headers_, header);
    }

private:
    CURL* handle_ = nullptr;
    curl_slist* headers_ = nullptr;
};
#endif

// ---- k6 脚本生成 ----
// URL / header / body 一律经 nlohmann dump 成 JSON 字符串字面量 —— 合法 JS，
// 转义问题一次解决。

std::string jsString(const std::string& s) { return json(s).dump(); }

std::string formUrlEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (c == ' ') out.push_back('+');
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

std::string serializeFormUrlEncoded(const std::vector<api::KeyValue>& fields) {
    std::string out;
    for (const auto& field : fields) {
        if (!field.enabled || field.key.empty()) continue;
        if (!out.empty()) out.push_back('&');
        out += formUrlEncode(field.key) + "=" + formUrlEncode(field.value);
    }
    return out;
}

std::string buildScript(const api::RequestSpec& spec, const api::LoadOptions& opts) {
    std::string headersObj = "{";
    bool first = true;
    for (const auto& h : spec.headers) {
        if (!h.enabled || h.key.empty()) continue;
        if (!first) headersObj += ", ";
        headersObj += jsString(h.key) + ": " + jsString(h.value);
        first = false;
    }
    headersObj += "}";

    const std::string bodyText = spec.bodyKind == api::BodyKind::FormUrlEncoded
        ? serializeFormUrlEncoded(spec.bodyFields) : spec.body;
    const std::string bodyJs = spec.bodyKind == api::BodyKind::None ? "null" : jsString(bodyText);
    const std::string timeout = std::format("{}s", opts.timeoutSec);

    std::string script;
    script += "import http from 'k6/http';\n\n";
    // summaryTrendStats：k6 的 Trend 汇总默认只带 avg/min/med/max/p(90)/p(95)，
    // p50 要用 med 键，p(99) 需显式声明才会出现在 values 里。
    script += "export const options = { vus: " + std::to_string(opts.vus) +
              ", duration: " + jsString(opts.duration) +
              ", summaryTrendStats: ['avg', 'min', 'med', 'max', 'p(90)', 'p(95)', 'p(99)'] };\n\n";
    script += "const kUrl = " + jsString(spec.url) + ";\n";
    script += "const kMethod = " + jsString(spec.method) + ";\n";
    script += "const kHeaders = " + headersObj + ";\n";
    script += "const kBody = " + bodyJs + ";\n\n";
    script += "export default function () {\n";
    script += "  http.request(kMethod, kUrl, kBody, { headers: kHeaders, timeout: " +
              jsString(timeout) + " });\n";
    script += "}\n\n";
    // handleSummary：把关键指标打成一行带前缀的 JSON 印到 stdout，宿主进程按前缀
    // 捞行解析 —— 比解析 k6 的文本 summary 稳，也比 --summary-export（已弃用）长寿。
    script += R"JS(
export function handleSummary(data) {
  const m = data.metrics || {};
  const val = (name, key) => (m[name] && m[name].values && m[name].values[key] !== undefined)
    ? m[name].values[key] : 0;
  const out = {
    requests: val('http_reqs', 'count'),
    rps: val('http_reqs', 'rate'),
    avg: val('http_req_duration', 'avg'),
    min: val('http_req_duration', 'min'),
    max: val('http_req_duration', 'max'),
    p50: val('http_req_duration', 'med'),
    p90: val('http_req_duration', 'p(90)'),
    p95: val('http_req_duration', 'p(95)'),
    p99: val('http_req_duration', 'p(99)'),
    failRate: val('http_req_failed', 'rate'),
  };
  return { stdout: '\nK6SUMMARY ' + JSON.stringify(out) + '\n' };
}
)JS";
    return script;
}

int currentPid() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

class K6Engine final : public api::LoadEngine {
public:
    explicit K6Engine(std::string binaryPath) : binary_(std::move(binaryPath)) {}

    ~K6Engine() override {
        stop();
        joinMonitor();
    }

    bool available() const override { return !binary_.empty(); }
    std::string binaryPath() const override { return binary_; }

    void start(const api::RequestSpec& spec, const api::LoadOptions& opts) override {
        if (running_.load()) {
            stop();
        }
        joinMonitor();  // 等上一次监视线程收尾（结果槽串行）
        {
            std::lock_guard lock(mutex_);
            output_.clear();
            summary_ = api::LoadSummary{};
            summaryReady_ = false;
        }
        stopRequested_.store(false);

        if (!available()) {
            failFast("未找到 k6 二进制（engines/ 或 PATH）");
            return;
        }

        // 脚本落临时目录（k6 只支持从文件/ stdin 读脚本；stdin 方案要再维护一根
        // 写管道，文件简单且便于用户排查）。
        scriptPath_ = std::filesystem::temp_directory_path() /
                      std::format("apitab-k6-{}.js", currentPid());
        {
            std::ofstream out(scriptPath_, std::ios::binary | std::ios::trunc);
            if (!out) {
                failFast("无法写入临时脚本: " + scriptPath_.string());
                return;
            }
            // opts.script 非空 = 用户在编辑器里自定义的脚本，原样跑；
            // 空 = 按 spec 自动生成（与 BuildScript 模板同一份逻辑）。
            out << (opts.script.empty() ? buildScript(spec, opts) : opts.script);
        }

        if (!spawn(spec.proxy)) {
            failFast("k6 进程启动失败");
            std::error_code ec;
            std::filesystem::remove(scriptPath_, ec);
            return;
        }

        startedAt_ = std::chrono::steady_clock::now();
        running_.store(true);
        try {
            monitor_ = std::thread([this] { monitorLoop(); });
        } catch (const std::exception& error) {
            // 线程起不来：子进程与管道由 RAII 兜底，引擎绝不能带着一个没人读
            // 输出、也没人回收的活子进程停在"已启动"状态。
            running_.store(false);
            stopRequested_.store(true);
            child_.terminate(false);
            child_.reap();
            readPipe_.close();
            std::error_code ec;
            std::filesystem::remove(scriptPath_, ec);
            failFast(std::string{"k6 监视线程创建失败: "} + error.what());
            return;
        }
    }

    void stop() override {
        if (!running_.load()) return;
        stopRequested_.store(true);
#ifdef _WIN32
        // Windows 没有可靠的跨进程 Ctrl+C：走 k6 REST API 优雅停止（handleSummary
        // 照常执行、summary 保留），API 不可达（没选到端口/尚未就绪）才退回
        // TerminateProcess；监视线程 3s 宽限后兜底强杀。
        if (!requestStopViaApi()) child_.terminate(/*graceful=*/false);
#else
        child_.terminate(/*graceful=*/true);  // POSIX=SIGINT；监视线程超时后强杀
#endif
    }

    bool running() const override { return running_.load(); }

    std::vector<std::string> drainOutput() override {
        std::lock_guard lock(mutex_);
        return std::exchange(output_, {});
    }

    bool takeSummary(api::LoadSummary& out) override {
        std::lock_guard lock(mutex_);
        if (!summaryReady_) return false;
        out = std::move(summary_);
        summary_ = api::LoadSummary{};
        summaryReady_ = false;
        return true;
    }

private:
    // ---- 子进程资源（RAII：见上方 ChildProcess / ChildPipe）----
    ChildProcess child_;
    ChildPipe readPipe_;

    void joinMonitor() {
        if (monitor_.joinable()) monitor_.join();
    }

    void failFast(std::string error) {
        std::lock_guard lock(mutex_);
        summary_ = api::LoadSummary{.ok = false, .error = std::move(error)};
        summaryReady_ = true;
        core::platform::requestUiUpdate();
    }

    // ---- spawn / terminate（平台分支）----

#ifdef _WIN32
    // 优雅停止：k6 REST API PATCH stopped:true（实测 handleSummary 照常执行、
    // K6SUMMARY 行照常打印）。返回 false = API 不可达，调用方退回 TerminateProcess。
    // 注意在 UI 线程同步执行：回环连接拒绝是即时的，超时上限 2s 只是极端兜底。
    bool requestStopViaApi() const {
        if (apiPort_ == 0) return false;
        CurlEasy curl;
        if (curl.get() == nullptr) return false;
        curl.addHeader("Content-Type: application/json");
        const std::string url = std::format("http://127.0.0.1:{}/v1/status", apiPort_);
        constexpr std::string_view body =
            R"({"data":{"type":"status","id":"default","attributes":{"stopped":true}}})";
        curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "PATCH");
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, curl.headers());
        // 回环请求必须直连：环境变量里的 http_proxy 不得把停止请求送去代理。
        curl_easy_setopt(curl.get(), CURLOPT_PROXY, "");
        curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 2000L);
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1000L);
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
                         +[](char*, size_t size, size_t nmemb, void*) { return size * nmemb; });
        const CURLcode rc = curl_easy_perform(curl.get());
        if (rc != CURLE_OK) return false;
        long code = 0;
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
        return code >= 200 && code < 300;
    }
#endif

    bool spawn(const std::string& proxy) {
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE readRaw = nullptr;
        HANDLE writeRaw = nullptr;
        if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) return false;
        // 局部 guard：下面任一提前 return 都自动关掉两端句柄（旧实现在宽字符
        // 转换失败时两处 return 都漏掉了刚建好的管道）。
        ChildPipe readPipe{readRaw};
        ChildPipe writePipe{writeRaw};
        SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe.get();
        si.hStdError = writePipe.get();
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

        const int binaryLength = MultiByteToWideChar(CP_UTF8, 0, binary_.data(),
                                                      static_cast<int>(binary_.size()), nullptr, 0);
        if (binaryLength <= 0) return false;
        std::wstring binaryWide(static_cast<std::size_t>(binaryLength), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, binary_.data(), static_cast<int>(binary_.size()),
                            binaryWide.data(), binaryLength);
        // REST API 是 Windows 优雅停止的唯一通道：选好端口才挂 --address；
        // 没选到（winsock 不可用）则 stop() 退回 TerminateProcess。
        apiPort_ = pickFreeLoopbackPort();
        std::wstring cmd = L"\"" + binaryWide + L"\" run --no-color";
        if (apiPort_ != 0)
            cmd += L" --address 127.0.0.1:" + std::to_wstring(apiPort_);
        cmd += L" \"" + scriptPath_.wstring() + L"\"";
        std::wstring envBlock = buildEnvironmentBlock(proxy);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                       envBlock.data(), nullptr, &si, &pi);
        // 写端随 writePipe 析构关闭：父进程必须放手，否则子进程退出后读端也等
        // 不到 EOF（句柄仍被父进程持有）。
        if (!ok) return false;
        child_.adopt(pi.hProcess);
        ::CloseHandle(pi.hThread);  // 线程句柄无人使用，立即关
        readPipe_.adopt(readPipe.release());
        return true;
#else
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;
        ChildPipe readPipe{pipefd[0]};
        ChildPipe writePipe{pipefd[1]};

        SpawnFileActions actions;
        ::posix_spawn_file_actions_adddup2(actions.get(), pipefd[1], STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(actions.get(), pipefd[1], STDERR_FILENO);
        ::posix_spawn_file_actions_addclose(actions.get(), pipefd[0]);

        std::string bin = binary_;
        std::string script = scriptPath_.string();
        std::vector<std::string> argsStorage{bin, "run", "--no-color", script};
        std::vector<char*> argv;
        for (auto& a : argsStorage) argv.push_back(a.data());
        argv.push_back(nullptr);

        // 环境表：继承父进程，代理变量按 spec.proxy 显式覆盖 —— 与 curl 引擎同一
        // 契约：空 = 直连（剥掉全部代理变量，不受环境里的 http_proxy 影响）；
        // 非空 = 注入 HTTP(S)_PROXY（Go 的 httpproxy 大小写都查，两种都写）并
        // 剥掉 NO_PROXY，避免环境里的 no_proxy 旁路用户显式配置的代理。
        std::vector<std::string> envStorage;
        static constexpr std::string_view kProxyKeys[] = {
            "http_proxy=", "https_proxy=", "HTTP_PROXY=", "HTTPS_PROXY=",
            "no_proxy=", "NO_PROXY="};
        for (char** e = environ; *e != nullptr; ++e) {
            const std::string_view entry{*e};
            const bool isProxy = std::ranges::any_of(kProxyKeys, [&](std::string_view key) {
                return entry.starts_with(key);
            });
            if (!isProxy) envStorage.emplace_back(entry);
        }
        if (!proxy.empty()) {
            envStorage.push_back("HTTP_PROXY=" + proxy);
            envStorage.push_back("HTTPS_PROXY=" + proxy);
            envStorage.push_back("http_proxy=" + proxy);
            envStorage.push_back("https_proxy=" + proxy);
        }
        std::vector<char*> envp;
        envp.reserve(envStorage.size() + 1);
        for (auto& entry : envStorage) envp.push_back(entry.data());
        envp.push_back(nullptr);

        pid_t child = kNoProcess;
        const int rc = ::posix_spawnp(&child, bin.c_str(), actions.get(), nullptr,
                                      argv.data(), envp.data());
        // 写端随 writePipe 析构关闭：父进程必须放手，否则 k6 退出后读端也等不到
        // EOF（写端仍被父进程持有 → poll 永不 POLLHUP → 监视线程挂死）。
        if (rc != 0) return false;
        child_.adopt(child);
        readPipe_.adopt(readPipe.release());
        return true;
#endif
    }

    // ---- 监视线程：读输出 → 收尾 → 解析 summary ----

    void pushLine(std::string line) {
        if (line.empty()) return;
        {
            std::lock_guard lock(mutex_);
            // K6SUMMARY 行不进输出队列，留给汇总解析（用户不该看到 JSON 噪音）。
            // 必须先于上限检查：队列满时丢掉汇总行会让一次成功的压测被报成
            // "k6 异常结束（未产生汇总）"。
            if (line.starts_with("K6SUMMARY ")) {
                summary_ = api::ParseSummaryLine(line);
                return;
            }
            if (output_.size() >= kMaxOutputLines) return;
            output_.push_back(std::move(line));
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastWake_ >= kWakeThrottle) {
            lastWake_ = now;
            core::platform::requestUiUpdate();
        }
    }

    void monitorLoop() {
        std::string pending;
        // stop() 发过 SIGINT 后的宽限计时（未发则保持 max，永不触发强杀）。
        auto killDeadline = std::chrono::steady_clock::time_point::max();

        // 读循环：POSIX 用 poll 带超时，stop 宽限期到 → 强杀。Windows 用
        // PeekNamedPipe 不阻塞取已到达字节 + WaitForSingleObject 200ms 轮询
        // 子进程退出，跑同一套宽限计时（REST 优雅停止失败 → 宽限到 → 强杀）。
#ifdef _WIN32
        bool processDone = false;
        for (;;) {
            DWORD avail = 0;
            if (!::PeekNamedPipe(readPipe_.get(), nullptr, 0, nullptr, &avail, nullptr)) {
                // 管道断开（写端关闭）：尝试把残留排干后退出。
                char buf[4096];
                DWORD n = 0;
                if (::ReadFile(readPipe_.get(), buf, sizeof(buf), &n, nullptr) && n > 0)
                    splitLines(pending, buf, n);
                break;
            }
            if (avail > 0) {
                char buf[4096];
                DWORD n = 0;
                const DWORD want = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
                if (!::ReadFile(readPipe_.get(), buf, want, &n, nullptr) || n == 0) break;
                splitLines(pending, buf, n);
                continue;  // 还有数据就先读完
            }
            if (processDone) break;  // 进程已退且管道排空
            const NativeProcess proc = child_.nativeHandle();
            if (proc != kNoProcess &&
                ::WaitForSingleObject(proc, 200) == WAIT_OBJECT_0) {
                processDone = true;  // 回到循环顶排干残留
                continue;
            }
            if (stopRequested_.load() && child_.running()) {
                const auto now = std::chrono::steady_clock::now();
                if (killDeadline == std::chrono::steady_clock::time_point::max()) {
                    killDeadline = now + kGracePeriod;  // 首次观察到 stop → 起宽限
                } else if (now >= killDeadline) {
                    child_.terminate(/*graceful=*/false);
                    killDeadline = std::chrono::steady_clock::time_point::max();  // 只杀一次
                }
            }
        }
#else
        for (;;) {
            pollfd pfd{readPipe_.get(), POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr == 0) {
                if (stopRequested_.load() && child_.running()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (killDeadline == std::chrono::steady_clock::time_point::max()) {
                        killDeadline = now + kGracePeriod;  // 首次观察到 stop → 起宽限
                    } else if (now >= killDeadline) {
                        child_.terminate(/*graceful=*/false);
                        killDeadline = std::chrono::steady_clock::time_point::max();  // 只杀一次
                    }
                }
                continue;
            }
            if (pr < 0) break;
            if (pfd.revents & (POLLHUP | POLLERR)) {
                // 排干残留后退出
                char buf[4096];
                const ssize_t n = ::read(readPipe_.get(), buf, sizeof(buf));
                if (n > 0) splitLines(pending, buf, static_cast<size_t>(n));
                break;
            }
            if (pfd.revents & POLLIN) {
                char buf[4096];
                const ssize_t n = ::read(readPipe_.get(), buf, sizeof(buf));
                if (n <= 0) break;
                splitLines(pending, buf, static_cast<size_t>(n));
            }
        }
#endif

        if (!pending.empty()) pushLine(std::move(pending));

        // 收尾：等退出码并回收子进程、关闭管道、删除临时脚本。全部经 RAII 类型，
        // 任何提前退出/异常都不会把这些资源留在引擎里。
        child_.reap();
        readPipe_.close();
        std::error_code ec;
        std::filesystem::remove(scriptPath_, ec);

        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - startedAt_)
                                   .count();
        {
            std::lock_guard lock(mutex_);
            summary_.durationSec = elapsed;
            if (!summary_.ok && summary_.error.empty()) {
                if (stopRequested_.load()) {
                    // 用户主动停止且 k6 没来得及打印 summary（杀太快）。
                    summary_.error = "已手动停止";
                } else {
                    summary_.error = "k6 异常结束（未产生汇总）";
                }
            }
            summaryReady_ = true;
        }
        running_.store(false);
        core::platform::requestUiUpdate();
    }

    void splitLines(std::string& pending, const char* buf, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!pending.empty()) pushLine(std::move(pending));
                pending.clear();
            } else {
                pending.push_back(c);
            }
        }
    }

    std::string binary_;
    std::filesystem::path scriptPath_;
    int apiPort_ = 0;  // k6 REST API 回环端口（Windows 优雅停止用；0 = 没选到）
    std::thread monitor_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point lastWake_{};

    std::mutex mutex_;
    std::vector<std::string> output_;
    api::LoadSummary summary_;
    bool summaryReady_ = false;
};

} // namespace

namespace api {

std::string BuildScript(const RequestSpec& spec, const LoadOptions& opts) {
    return buildScript(spec, opts);
}

LoadSummary ParseSummaryLine(std::string_view line) {
    LoadSummary s;
    constexpr std::string_view kPrefix = "K6SUMMARY ";
    if (!line.starts_with(kPrefix)) return s;
    const auto j = json::parse(line.substr(kPrefix.size()), nullptr, false);
    if (j.is_discarded()) return s;
    auto num = [&](const char* key) { return j.value(key, 0.0); };
    s.ok = true;
    s.requests = j.value("requests", std::int64_t{0});
    s.rps = num("rps");
    s.avgMs = num("avg");
    s.minMs = num("min");
    s.maxMs = num("max");
    s.p50Ms = num("p50");
    s.p90Ms = num("p90");
    s.p95Ms = num("p95");
    s.p99Ms = num("p99");
    s.failRate = num("failRate");
    return s;
}

} // namespace api

std::unique_ptr<api::LoadEngine> makeK6Engine(std::string binaryPath) {
    return std::make_unique<K6Engine>(std::move(binaryPath));
}
