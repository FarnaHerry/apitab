// control.cpp — 运行中实例的控制面服务端（见 control.h 与
// docs/plans/runtime-control-surface.md）。
//
// 线程契约：接受/读写在**控制面线程**（本文件起的 io_context 线程），命令执行在
// **应用线程**（DispatchToApplicationThread），两条线程之间只交换一个 Exchange
// （mutex + condition_variable），不共享 store/State。命令实现本身（cli::run）
// 只在应用线程串行执行，所以它内部的文件级输出汇是安全的。
#include <huxerui/huxerui.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "cli.h"
#include "control.h"
#include "log.h"
#include "ui/lightweight.h"

import asio;
import nlohmann.json;
import apitab.config;

namespace apitab::control {
namespace {

using json = nlohmann::json;

// 控制面单条命令的等待上限：命令在应用线程跑（阻塞式读库），超时说明实例卡住，
// 与其把客户端一起吊死，不如回一个明确错误。
constexpr auto kCommandTimeout = std::chrono::seconds{60};
// 单条连接读/写的兜底超时，防止半开连接把控制面线程占住（退出时 join 不挂）。
constexpr int kSocketTimeoutSeconds = 5;

// ---- 端点文件 --------------------------------------------------------------

std::string RuntimeDirectory() {
#ifdef _WIN32
    return {};
#else
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    return dir != nullptr && *dir != '\0' ? std::string{dir} : std::string{};
#endif
}

// 端点文件放运行目录（POSIX）以避免写用户数据目录；Windows 没有对应概念，回落
// 数据目录。两边都只有当前用户可读（POSIX 显式 0600）。
std::filesystem::path EndpointPath() {
    const std::string runtime = RuntimeDirectory();
    if (!runtime.empty()) return std::filesystem::path(runtime) / "apitab-control.json";
    return cfg::dataDir() / "control.json";
}

// 随机 token：控制面只绑回环，token 用来挡住同机其它用户的进程。
std::string MakeToken() {
    std::random_device device;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(48);
    for (int i = 0; i < 48; ++i) token.push_back(kHex[device() & 0xF]);
    return token;
}

// 端点文件：Start() 成功后创建，析构（进程退出/应用关闭）删除。
class EndpointFile {
public:
    EndpointFile() = default;
    ~EndpointFile() {
        if (path_.empty()) return;
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }

    EndpointFile(const EndpointFile&) = delete;
    EndpointFile& operator=(const EndpointFile&) = delete;

    bool Write(int port, const std::string& token, std::string& error) {
        const std::filesystem::path path = EndpointPath();
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        const json payload{{"port", port}, {"token", token}, {"pid", static_cast<std::int64_t>(
#ifdef _WIN32
            static_cast<long long>(::GetCurrentProcessId())
#else
            static_cast<long long>(::getpid())
#endif
        )}};
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "无法写入控制面端点文件: " + path.string();
            return false;
        }
        out << payload.dump();
        out.close();
#ifndef _WIN32
        ::chmod(path.c_str(), 0600);  // 只有本用户可读
#endif
        path_ = path;
        return true;
    }

private:
    std::filesystem::path path_;
};

// ---- 输出汇 ----------------------------------------------------------------

class CollectSink final : public cli::Sink {
public:
    void Out(std::string_view line) override {
        stdout_.append(line);
        stdout_.push_back('\n');
    }
    void Err(std::string_view line) override {
        stderr_.append(line);
        stderr_.push_back('\n');
    }

    std::string stdout_;
    std::string stderr_;
};

// ---- 跨线程交换 ------------------------------------------------------------

struct Exchange {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    int exit_code = 1;
    std::string stdout_;
    std::string stderr_;
};

// 在应用线程上跑一条命令。异常也落成 stderr + 退出码 1，绝不让异常穿过
// DispatchToApplicationThread 的调用栈。
void RunCommandOnApplicationThread(std::vector<std::string> args,
                                   std::shared_ptr<Exchange> exchange) {
    CollectSink sink;
    int code = 1;
    const auto started_at = std::chrono::steady_clock::now();
    // 审计用的参数快照（下面 args 会被 move 走）。
    std::string audit_args;
    for (const std::string& item : args) {
        if (!audit_args.empty()) audit_args.push_back(' ');
        audit_args += item;
    }
    try {
        // 轻量模式属于"实例进程形态"操作，不是数据命令：由控制面自己处理
        // （cli::run 只管 apitab 领域命令）。
        // 就绪探测：与 lightweight 一样由控制面自己处理。它必须经 poster 投递，
        // 所以能回答"应用线程投递口已挂载"——客户端据此判断实例是否真的可服务
        // （端点文件在监听成功后立刻写出，但投递口要等首次组合才挂上）。
        if (!args.empty() && args.front() == "ping") {
            sink.Out("pong");
            code = 0;
        } else if (!args.empty() && args.front() == "lightweight") {
            const bool off = args.size() > 1 && args[1] == "off";
            const ui::LightweightResult result =
                off ? ui::ExitLightweightMode() : ui::EnterLightweightMode();
            sink.Out(result.message);
            code = result.ok ? 0 : 1;
        } else {
            code = cli::run(args, sink);
        }
    } catch (const std::exception& error) {
        if (!sink.stderr_.empty()) sink.stderr_.push_back('\n');
        sink.stderr_ += std::string{"命令执行异常: "} + error.what();
    } catch (...) {
        sink.stderr_ += "命令执行未知异常";
    }
    // 审计/诊断落**文件**（不进 SQLite：日志是高频追加的运维信息，塞进库会与领域
    // 读写抢 WAL 的唯一写者，见 CLAUDE.md 关键约定 10）。
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    if (code == 0) {
        log::Info("control cmd [" + audit_args + "] exit=0 " + std::to_string(elapsed_ms) + "ms");
    } else {
        std::string detail = sink.stderr_;
        if (const auto newline = detail.find('\n'); newline != std::string::npos) {
            detail.resize(newline);
        }
        log::Warn("control cmd [" + audit_args + "] exit=" + std::to_string(code) + " " +
                  std::to_string(elapsed_ms) + "ms" +
                  (detail.empty() ? std::string{} : " :: " + detail));
    }
    {
        std::lock_guard lock{exchange->mutex};
        exchange->exit_code = code;
        exchange->stdout_ = std::move(sink.stdout_);
        exchange->stderr_ = std::move(sink.stderr_);
        exchange->done = true;
    }
    exchange->changed.notify_all();
}

// ---- 服务端 ----------------------------------------------------------------

class ControlServer {
public:
    explicit ControlServer(std::shared_ptr<ApplicationPoster> poster) : poster_(std::move(poster)) {}
    ~ControlServer() { Stop(); }

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    bool Start(std::string& error) {
        asio::error_code ec;
        const asio::ip::tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1", ec), 0};
        if (ec) {
            error = std::string{"控制面地址解析失败: "} + ec.message();
            return false;
        }
        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            error = std::string{"控制面监听套接字创建失败: "} + ec.message();
            return false;
        }
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        acceptor_.bind(endpoint, ec);
        if (ec) {
            error = std::string{"控制面绑定 127.0.0.1 失败: "} + ec.message();
            return false;
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error = std::string{"控制面监听失败: "} + ec.message();
            return false;
        }
        const int port = static_cast<int>(acceptor_.local_endpoint().port());
        token_ = MakeToken();
        if (!endpoint_file_.Write(port, token_, error)) return false;
        stopping_.store(false);
        thread_ = std::thread([this] { Serve(); });
        return true;
    }

    void Stop() {
        if (stopping_.exchange(true)) return;
        asio::error_code ec;
        acceptor_.close(ec);  // 唤醒阻塞中的 accept，服务线程随后退出
        if (thread_.joinable()) thread_.join();
    }

private:
    void Serve() {
        for (;;) {
            asio::ip::tcp::socket socket{context_};
            asio::error_code ec;
            acceptor_.accept(socket, ec);
            if (ec) return;  // acceptor 已关闭 → 退出
            SetSocketTimeouts(socket);
            HandleConnection(socket);
        }
    }

    static void SetSocketTimeouts(asio::ip::tcp::socket& socket) {
#ifdef _WIN32
        const DWORD timeout = kSocketTimeoutSeconds * 1000;
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        const timeval timeout{kSocketTimeoutSeconds, 0};
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    }

    // 一次一条连接，串行处理：控制面是本机单用户的调试/自动化通道，不需要并发。
    // 解析用手工缓冲（本构建定义 ASIO_NO_IOSTREAM，asio::streambuf/read_until 不可用）。
    void HandleConnection(asio::ip::tcp::socket& socket) {
        char chunk[4096];
        std::string request;
        asio::error_code ec;
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const std::size_t n = socket.read_some(asio::buffer(chunk), ec);
            if (ec) return;
            request.append(chunk, n);
            header_end = request.find("\r\n\r\n");
            if (request.size() > 64u * 1024u) return;  // 头块上限，防滥用
        }

        const std::string headers = request.substr(0, header_end);
        std::string authorization;
        std::size_t content_length = 0;
        std::size_t line_start = headers.find("\r\n");  // 跳过请求行
        line_start = line_start == std::string::npos ? headers.size() : line_start + 2;
        while (line_start < headers.size()) {
            const std::size_t line_end = headers.find("\r\n", line_start);
            const std::string line = headers.substr(
                line_start, (line_end == std::string::npos ? headers.size() : line_end) - line_start);
            line_start = line_end == std::string::npos ? headers.size() : line_end + 2;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
            if (name == "Authorization") authorization = value;
            else if (name == "Content-Length") content_length = static_cast<std::size_t>(std::atoll(value.c_str()));
        }

        if (authorization != "Bearer " + token_) {
            log::Warn("控制面拒绝连接：token 不匹配或缺失");
            SendJson(socket, 401, json{{"error", "unauthorized"}});
            return;
        }

        std::string body = request.substr(header_end + 4);
        while (body.size() < content_length) {
            const std::size_t n = socket.read_some(asio::buffer(chunk), ec);
            if (ec) return;
            body.append(chunk, n);
        }
        body.resize(content_length);

        std::vector<std::string> args;
        try {
            const json parsed = json::parse(body);
            for (const auto& item : parsed.at("args")) args.push_back(item.get<std::string>());
        } catch (const std::exception& error) {
            SendJson(socket, 400, json{{"error", std::string{"请求解析失败: "} + error.what()}});
            return;
        }

        auto exchange = std::make_shared<Exchange>();
        if (!poster_->PostTask([args = std::move(args), exchange]() mutable {
                RunCommandOnApplicationThread(std::move(args), std::move(exchange));
            })) {
            SendJson(socket, 503, json{{"error", "实例未就绪（应用线程投递口未挂载）"}});
            return;
        }

        std::unique_lock lock{exchange->mutex};
        if (!exchange->changed.wait_for(lock, kCommandTimeout, [&] { return exchange->done; })) {
            SendJson(socket, 504, json{{"error", "命令超时（实例未在预期时间内返回）"}});
            return;
        }
        json response{{"exit", exchange->exit_code},
                      {"stdout", exchange->stdout_},
                      {"stderr", exchange->stderr_}};
        lock.unlock();
        SendJson(socket, 200, response);
    }

    void SendJson(asio::ip::tcp::socket& socket, int status, const json& payload) {
        const std::string body = payload.dump();
        std::string response = "HTTP/1.1 " + std::to_string(status) + " OK\r\n";
        response += "Content-Type: application/json\r\n";
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;
        asio::error_code ec;
        asio::write(socket, asio::buffer(response), ec);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    }

    std::shared_ptr<ApplicationPoster> poster_;
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_{context_};
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::string token_;
    EndpointFile endpoint_file_;
};

} // namespace

void ApplicationPoster::Set(Post post) {
    std::lock_guard lock{mutex_};
    post_ = std::move(post);
}

void ApplicationPoster::Clear() {
    std::lock_guard lock{mutex_};
    post_ = nullptr;
}

bool ApplicationPoster::PostTask(std::function<void()> task) const {
    Post post;
    {
        std::lock_guard lock{mutex_};
        post = post_;
    }
    if (!post) return false;
    post(std::move(task));
    return true;
}

std::string EndpointFilePath() { return EndpointPath().string(); }

bool ReadEndpoint(int& port, std::string& token, std::string& error) {
    const std::filesystem::path path = EndpointPath();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "apitab 未在运行（找不到控制面端点 " + path.string() +
                "）。先启动 apitab，或用 apitab --ensure 拉起。";
        return false;
    }
    try {
        json payload;
        input >> payload;
        port = payload.at("port").get<int>();
        token = payload.at("token").get<std::string>();
    } catch (const std::exception& parse_error) {
        error = "控制面端点文件不可用（" + path.string() + "）: " + parse_error.what();
        return false;
    }
    return true;
}

void InstallControlServer(huxerui::ApplicationContext& context) {
    // 投递口先 Provide：根组合挂载时会 UseService 取它并注入 TaskScope::Post。
    auto poster = std::make_shared<ApplicationPoster>();
    context.Provide(poster);
    auto server = std::make_shared<ControlServer>(poster);
    std::string error;
    if (!server->Start(error)) {
        // 控制面起不来不该拖垮 GUI：agent 侧会看到"端点不可达"，GUI 照常可用。
        log::Error("控制面启动失败: " + error);
        std::fprintf(stderr, "apitab 控制面启动失败: %s\n", error.c_str());
        return;
    }
    log::Info("控制面已启动: " + EndpointFilePath() + "（pid " +
              std::to_string(static_cast<long long>(
#ifdef _WIN32
                  ::GetCurrentProcessId()
#else
                  ::getpid()
#endif
                  )) + "）");
    context.Provide(server);  // 活到应用关闭 → 停线程 + 删端点文件
}

} // namespace apitab::control
