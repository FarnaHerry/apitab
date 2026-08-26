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
            out << buildScript(spec, opts);
        }

        if (!spawn()) {
            failFast("k6 进程启动失败");
            std::error_code ec;
            std::filesystem::remove(scriptPath_, ec);
            return;
        }

        startedAt_ = std::chrono::steady_clock::now();
        running_.store(true);
        monitor_ = std::thread([this] { monitorLoop(); });
    }

    void stop() override {
        if (!running_.load()) return;
        stopRequested_.store(true);
        terminateChild(/*graceful=*/true);  // POSIX=SIGINT；监视线程超时后强杀
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
    // ---- 子进程句柄（平台各一份）----
#ifdef _WIN32
    HANDLE childProcess_ = nullptr;
    HANDLE readPipe_ = nullptr;
#else
    pid_t childPid_ = -1;
    int readFd_ = -1;
#endif

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
        HANDLE writePipe = nullptr;
        if (!CreatePipe(&readPipe_, &writePipe, &sa, 0)) return false;
        SetHandleInformation(readPipe_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
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
        CloseHandle(writePipe);
        if (!ok) {
            CloseHandle(readPipe_);
            readPipe_ = nullptr;
            return false;
        }
        childProcess_ = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
#else
        int pipefd[2];
        if (::pipe(pipefd) != 0) return false;

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);

        std::string bin = binary_;
        std::string script = scriptPath_.string();
        std::vector<std::string> argsStorage{bin, "run", "--no-color", script};
        std::vector<char*> argv;
        for (auto& a : argsStorage) argv.push_back(a.data());
        argv.push_back(nullptr);

        const int rc = posix_spawnp(&childPid_, bin.c_str(), &actions, nullptr,
                                    argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[1]);
        if (rc != 0) {
            ::close(pipefd[0]);
            childPid_ = -1;
            return false;
        }
        readFd_ = pipefd[0];
        return true;
#endif
    }

    // graceful=true：POSIX 发 SIGINT 让 k6 优雅收尾（仍会打印 summary）；
    // 监视线程宽限期后强杀。Windows 无可靠的跨进程 Ctrl+C，直接 Terminate。
    void terminateChild(bool graceful) {
#ifdef _WIN32
        (void)graceful;
        if (childProcess_) TerminateProcess(childProcess_, 1);
#else
        if (childPid_ > 0) ::kill(childPid_, graceful ? SIGINT : SIGKILL);
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
            if (!ReadFile(readPipe_, buf, sizeof(buf), &n, nullptr) || n == 0) break;
            splitLines(pending, buf, n);
#else
            pollfd pfd{readFd_, POLLIN, 0};
            const int pr = ::poll(&pfd, 1, 200);
            if (pr == 0) {
                if (stopRequested_.load() && childPid_ > 0) {
                    const auto now = std::chrono::steady_clock::now();
                    if (killDeadline == std::chrono::steady_clock::time_point::max()) {
                        killDeadline = now + kGracePeriod;  // 首次观察到 stop → 起宽限
                    } else if (now >= killDeadline) {
                        ::kill(childPid_, SIGKILL);
                        killDeadline = std::chrono::steady_clock::time_point::max();  // 只杀一次
                    }
                }
                continue;
            }
            if (pr < 0) break;
            if (pfd.revents & (POLLHUP | POLLERR)) {
                // 排干残留后退出
                char buf[4096];
                const ssize_t n = ::read(readFd_, buf, sizeof(buf));
                if (n > 0) splitLines(pending, buf, static_cast<size_t>(n));
                break;
            }
            if (pfd.revents & POLLIN) {
                char buf[4096];
                const ssize_t n = ::read(readFd_, buf, sizeof(buf));
                if (n <= 0) break;
                splitLines(pending, buf, static_cast<size_t>(n));
            }
#endif
        }

        if (!pending.empty()) pushLine(std::move(pending));

        // 收尾：等退出码，清理句柄与临时脚本。
#ifdef _WIN32
        if (childProcess_) {
            WaitForSingleObject(childProcess_, INFINITE);
            CloseHandle(childProcess_);
            childProcess_ = nullptr;
        }
        if (readPipe_) {
            CloseHandle(readPipe_);
            readPipe_ = nullptr;
        }
#else
        int status = 0;
        if (childPid_ > 0) {
            while (::waitpid(childPid_, &status, 0) < 0 && errno == EINTR) {}
            childPid_ = -1;
        }
        if (readFd_ >= 0) {
            ::close(readFd_);
            readFd_ = -1;
        }
#endif
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

std::unique_ptr<api::LoadEngine> makeK6Engine(std::string binaryPath) {
    return std::make_unique<K6Engine>(std::move(binaryPath));
}
