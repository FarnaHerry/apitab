// k6_engine.cpp — apitab.k6_engine 实现单元。
//
// 进程模型（对齐 tinynext aria2 引擎）：start() 在 UI 线程调用，posix_spawn /
// CreateProcess 拉起 k6 子进程（stdout+stderr 合并进一根管道），监视线程流式读
// 输出 —— k6 的进度行用 \r 原地刷新，统一按 \r / \n 拆行入队，节流 150ms
// requestUiUpdate() 唤醒 UI。EOF 后 waitpid / WaitForSingleObject 收尾，
// 从输出里捞脚本 handleSummary 打印的 `K6SUMMARY {json}` 行解析指标。
// stop()：POSIX 先 SIGINT（k6 收到后会优雅收尾并照常打印 summary），监视线程
// 3s 后未退出再 SIGKILL；Windows 直接 TerminateProcess。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stringapiset.h>
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

api::LoadSummary parseSummaryLine(std::string_view line) {    api::LoadSummary s;
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
        lastError_.clear();

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

        if (!spawn()) {
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
        child_.terminate(/*graceful=*/true);  // POSIX=SIGINT；监视线程超时后强杀
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

    bool spawn() {
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
        std::wstring cmd = L"\"" + binaryWide +
                           L"\" run --no-color \"" + scriptPath_.wstring() + L"\"";
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
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

        pid_t child = kNoProcess;
        const int rc = ::posix_spawnp(&child, bin.c_str(), actions.get(), nullptr,
                                      argv.data(), environ);
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
            if (output_.size() >= kMaxOutputLines) return;
            // K6SUMMARY 行不进输出队列，留给汇总解析（用户不该看到 JSON 噪音）。
            if (line.starts_with("K6SUMMARY ")) {
                summary_ = parseSummaryLine(line);
                return;
            }
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

        // 读循环：POSIX 用 poll 带超时，stop 宽限期到 → 强杀；Windows ReadFile
        // 阻塞读，TerminateProcess 后管道自然 EOF。
        for (;;) {
#ifdef _WIN32
            char buf[4096];
            DWORD n = 0;
            if (!ReadFile(readPipe_.get(), buf, sizeof(buf), &n, nullptr) || n == 0) break;
            splitLines(pending, buf, n);
#else
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
#endif
        }

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
    std::thread monitor_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::chrono::steady_clock::time_point startedAt_;
    std::chrono::steady_clock::time_point lastWake_{};
    std::string lastError_;

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

} // namespace api

std::unique_ptr<api::LoadEngine> makeK6Engine(std::string binaryPath) {
    return std::make_unique<K6Engine>(std::move(binaryPath));
}
