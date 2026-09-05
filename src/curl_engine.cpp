// curl_engine.cpp — apitab.curl_engine 实现单元。
//
// 线程模型：常驻工作线程 + 条件变量队列。send() 在 UI 线程调用、纯入队立即
// 返回——绝不在 UI 线程 join 工作线程（旧实现 send 时 join 上一条传输，端点
// 挂起时整条 UI 线程被拖死）。工作线程串行消费队列：新请求到来时协作打断在途
// 传输（CURLOPT_XFERINFOFUNCTION 每帧查原子标志，curl 返回
// CURLE_ABORTED_BY_CALLBACK），被取代/被取消请求的结果按代际丢弃，不会冒充新
// 请求的结果。完成后写互斥保护的结果槽并 requestUiUpdate() 唤醒 UI 一帧，UI
// 轮询 takeResponse() 取走。
//
// 健壮性保证：
// - 任何端点（DNS 失败 / 连接拒绝 / 不可路由）都在 timeoutSec 内返回错误结果
//   （CURLOPT_TIMEOUT 覆盖 DNS+连接+传输全程，CONNECTTIMEOUT 单独兜底连接段）；
//   唯一例外是 SSE：头部块结束时识别 text/event-stream 即关闭总超时（长连接
//   推流本身永不"完成"），取消仍走 XFERINFO 协作打断；
// - 工作线程全程 try/catch：任何异常都落成错误结果写进结果槽，线程不静默死亡
//   （否则结果槽永远不填，UI 永远停在"发送中…"）；
// - curl 回调 noexcept：异常不得穿过 libcurl 的 C 栈帧，出错即中断本次传输；
// - send 递增代际后立即清结果槽：上次取消残留（如未被取走的"已取消"）不会被
//   新请求的轮询误取；cancel 丢弃排队请求并递增代际作废在途结果。
//
// 流式进度：正文/头部在传输期直接累积进 progress_ 槽（写回调持锁追加），UI 经
// takeProgress 每 30ms 取全量快照增量呈现（SSE 逐条/逐字到达都能跟上）；传输
// 结束由 finish 把累积内容并入最终结果槽并清空进度槽。
module;

#include <curl/curl.h>

// 唤醒 UI 一帧（eui 提供）。前向声明必须放全局模块片段 —— 写在模块域内会被
// 附加模块名修饰（@apitab.curl_engine），链接不到 eui 的符号。
namespace core::platform { void requestUiUpdate(); }

module apitab.curl_engine;

import std;
import apitab.api_engine;

namespace {

// 响应体上限：API 调试场景 32 MiB 足够，防止打满内存（截断后在 body 尾部标注）。
constexpr std::size_t kMaxBodyBytes = 32u * 1024u * 1024u;

// 传输期共享上下文：正文与头部直接累积进引擎的 progress_ 槽（写路径持锁），
// UI 线程 takeProgress 读快照；blockStart 标记当前头部块起点（重定向会产生
// 多个头部块，SSE 检测只认最后一块的 Content-Type）。
struct TransferCtx {
    std::mutex* mutex;
    api::ResponseView* progress;
    bool* progressDirty;
    CURL* easy;
    std::atomic<bool>* isEventStream;
    bool* truncated;        // 仅工作线程读写
    std::size_t blockStart = 0;  // 仅工作线程读写
};

size_t onBodyWrite(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    // noexcept：异常绝不能穿过 libcurl 的 C 栈帧；失败返回错误值中断本次传输。
    try {
        const size_t n = size * nmemb;
        auto* ctx = static_cast<TransferCtx*>(userdata);
        std::lock_guard lock(*ctx->mutex);
        std::string& body = ctx->progress->body;
        const size_t room = kMaxBodyBytes - std::min(body.size(), kMaxBodyBytes);
        const size_t take = std::min(n, room);
        body.append(ptr, take);
        if (take < n) *ctx->truncated = true;
        *ctx->progressDirty = true;
        return n;  // 吞掉全部（不截断传输本身，只截断留存）
    } catch (...) {
        return CURL_WRITEFUNC_ERROR;
    }
}

size_t onHeaderLine(char* ptr, size_t size, size_t nmemb, void* userdata) noexcept {
    // noexcept：异常绝不能穿过 libcurl 的 C 栈帧；返回值不等于入参字节数即中断传输。
    try {
        const size_t n = size * nmemb;
        auto* ctx = static_cast<TransferCtx*>(userdata);
        std::string line(ptr, n);
        // 去掉行尾 \r\n；跳过状态行（HTTP/...）。
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) {
            // 头部块结束：拿到状态行后快照进度（重定向/100 Continue 的中间块
            // 会被后续块覆盖）。SSE 长连接在此关闭总超时——传输中改
            // CURLOPT_TIMEOUT 立即生效；万一未生效也只是退回原超时语义。
            bool sse = false;
            {
                std::lock_guard lock(*ctx->mutex);
                for (std::size_t i = ctx->blockStart; i < ctx->progress->headers.size(); ++i) {
                    std::string key = ctx->progress->headers[i].key;
                    std::ranges::transform(key, key.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    if (key != "content-type") continue;
                    std::string value = ctx->progress->headers[i].value;
                    std::ranges::transform(value, value.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    sse = value.find("text/event-stream") != std::string::npos;
                }
                ctx->blockStart = ctx->progress->headers.size();
            }
            long status = 0;
            curl_easy_getinfo(ctx->easy, CURLINFO_RESPONSE_CODE, &status);
            if (status != 0) {
                if (sse && !ctx->isEventStream->exchange(true))
                    curl_easy_setopt(ctx->easy, CURLOPT_TIMEOUT, 0L);
                std::lock_guard lock(*ctx->mutex);
                ctx->progress->status = status;
                *ctx->progressDirty = true;
            }
            return n;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos && !line.starts_with("HTTP/")) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // value 前导空格
            const auto pos = value.find_first_not_of(' ');
            if (pos != std::string::npos) value.erase(0, pos);
            std::lock_guard lock(*ctx->mutex);
            ctx->progress->headers.push_back({std::move(key), std::move(value), true});
        }
        return n;
    } catch (...) {
        return 0;
    }
}

std::string formUrlEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if (c == ' ') out.push_back('+');
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out.push_back(static_cast<char>(c));
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0xF]); }
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

std::vector<std::pair<std::string, std::string>> parseFormData(std::string_view body) {
    std::vector<std::pair<std::string, std::string>> fields;
    std::size_t start = 0;
    while (start <= body.size()) {
        const std::size_t end = body.find('\n', start);
        std::string_view line = body.substr(start, end == std::string_view::npos ? body.size() - start : end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        const std::size_t equal = line.find('=');
        if (equal != std::string_view::npos) {
            std::string key(line.substr(0, equal));
            std::string value(line.substr(equal + 1));
            if (!key.empty()) fields.emplace_back(std::move(key), std::move(value));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}

class CurlEngine final : public api::ApiEngine {
public:
    CurlEngine() {
        // 先完成 curl 全局初始化再启动工作线程（初始化非线程安全，
        // 线程启动后一旦有 send 到达就会调 curl_easy_init）。
        curl_global_init(CURL_GLOBAL_DEFAULT);
        worker_ = std::thread([this] { workerLoop(); });
    }
    ~CurlEngine() override {
        {
            std::lock_guard lock(queueMutex_);
            quit_ = true;
            cancelled_.store(true);  // 打断在途传输，让 join 尽快返回
        }
        queueCv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    // 纯入队，立即返回：只保留最新一条待跑规格；在途请求由工作线程自己协作
    // 打断并丢弃其结果（代际不符），UI 线程永不等待传输。
    void send(const api::RequestSpec& spec) override {
        {
            std::lock_guard lock(queueMutex_);
            queued_ = spec;
            ++generation_;
            cancelled_.store(true);  // 打断在途传输（若有）
        }
        {   // 递增代际后立即清结果槽与进度槽：上次取消残留的结果（如未被 UI
            // 取走的"已取消"）不得被新请求的轮询误取。在途旧结果随后也会因代际
            // 不符被 finish 丢弃。锁序 queueMutex_ → resultMutex_ 单向，无死锁。
            std::lock_guard rlock(resultMutex_);
            pending_ = api::ResponseView{};
            resultReady_ = false;
            progress_ = api::ResponseView{};
            progressDirty_ = false;
        }
        queueCv_.notify_one();
    }

    void cancel() override {
        std::lock_guard lock(queueMutex_);
        queued_.reset();   // 排队中的请求直接丢弃，取消后不再执行
        ++generation_;     // 在途结果按代际作废（不投递"已取消"残留进结果槽）
        cancelled_.store(true);  // 协作打断在途传输
    }

    bool busy() const override {
        if (busy_.load()) return true;
        std::lock_guard lock(queueMutex_);
        return queued_.has_value();
    }

    bool takeProgress(api::ResponseView& out) override {
        std::lock_guard lock(resultMutex_);
        if (!progressDirty_) return false;
        out = progress_;  // 全量快照；正文按 32 MiB 上限截断
        progressDirty_ = false;
        return true;
    }

    bool takeResponse(api::ResponseView& out) override {
        std::lock_guard lock(resultMutex_);
        if (!resultReady_) return false;
        out = std::move(pending_);
        pending_ = api::ResponseView{};
        resultReady_ = false;
        return true;
    }

private:
    void workerLoop() {
        while (true) {
            api::RequestSpec spec;
            std::uint64_t generation = 0;
            {
                std::unique_lock lock(queueMutex_);
                queueCv_.wait(lock, [this] { return quit_ || queued_.has_value(); });
                if (quit_) return;
                spec = std::move(*queued_);
                queued_.reset();
                generation = generation_.load();
                // 锁内复位取消标志：send 的置位要么在本复位之前（打断的是上一条
                // 传输，已被消费），要么在本复位之后（打断本条），不会误伤新请求。
                cancelled_.store(false);
                isEventStream_.store(false);
                transferTruncated_ = false;
                busy_.store(true);
            }
            {   // 丢弃上一请求遗留的结果槽与进度槽（已被新请求取代）
                std::lock_guard rlock(resultMutex_);
                pending_ = api::ResponseView{};
                resultReady_ = false;
                progress_ = api::ResponseView{};
                progressDirty_ = false;
            }
            runSafely(spec, generation);  // 内部全 catch，结果槽必被填写
        }
    }

    // run() 的异常兜底：工作线程上任何异常都落成错误结果投递（代际允许时），
    // 线程本身绝不因未捕获异常死亡——否则结果槽永远不填，UI 永远停在"发送中…"。
    void runSafely(const api::RequestSpec& spec, std::uint64_t generation) noexcept {
        try {
            run(spec, generation);
        } catch (const std::exception& e) {
            api::ResponseView result;
            result.error = std::string{"引擎内部异常: "} + e.what();
            finish(std::move(result), generation);
        } catch (...) {
            api::ResponseView result;
            result.error = "引擎内部未知异常";
            finish(std::move(result), generation);
        }
    }

    static int onProgress(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        const auto* self = static_cast<const CurlEngine*>(clientp);
        return self->cancelled_.load() ? 1 : 0;  // 非 0 → CURLE_ABORTED_BY_CALLBACK
    }

    void run(const api::RequestSpec& spec, std::uint64_t generation) {
        api::ResponseView result;

        CURL* easy = curl_easy_init();
        if (!easy) {
            result.error = "curl_easy_init failed";
            finish(std::move(result), generation);
            return;
        }

        TransferCtx transfer{&resultMutex_, &progress_, &progressDirty_, easy,
                             &isEventStream_, &transferTruncated_};
        struct curl_slist* headerList = nullptr;
        curl_mime* mime = nullptr;
        for (const auto& h : spec.headers) {
            if (!h.enabled || h.key.empty()) continue;
            const std::string line = h.key + ": " + h.value;
            headerList = curl_slist_append(headerList, line.c_str());
        }

        for (const auto& cookie : spec.cookies) {
            if (!cookie.enabled || cookie.key.empty()) continue;
            const std::string line = cookie.key + "=" + cookie.value;
            headerList = curl_slist_append(headerList, ("Cookie: " + line).c_str());
        }
        curl_easy_setopt(easy, CURLOPT_URL, spec.url.c_str());
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, spec.method.c_str());
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, spec.followRedirects ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &onBodyWrite);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &onHeaderLine);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer);
        // timeoutSec 覆盖 DNS+连接+传输全程（含 threaded resolver 的解析等待）；
        // CONNECTTIMEOUT 单独给连接段兜底。非法值（<=0 表示"无限"）一律钳制到
        // 30s——引擎契约是"任何端点都在有限时间内返回"。SSE 长连接是唯一例外：
        // 头部块识别 text/event-stream 后 onHeaderLine 会把总超时关掉。
        const long timeoutSec = spec.timeoutSec > 0 ? spec.timeoutSec : 30L;
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, timeoutSec);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, timeoutSec);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);  // 多线程必须（禁用信号超时）
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "apitab/0.1");
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");  // 自动解压 gzip/br
        curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &onProgress);
        curl_easy_setopt(easy, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);  // 启用 xferinfo 回调
        // CURLOPT_POSTFIELDS 只保存指针，不复制内容；body 必须活到 perform() 完成。
        std::string body;
        if (spec.bodyKind != api::BodyKind::None) {
            body = spec.body;
            if (spec.bodyKind == api::BodyKind::FormUrlEncoded) {
                body = serializeFormUrlEncoded(spec.bodyFields);
            }
            // allowJsonComments 是 UI/编辑层关注点（发送前由上层剥离注释），引擎不处理。
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(body.size()));
            if (spec.bodyKind == api::BodyKind::Json || spec.bodyKind == api::BodyKind::GraphQL) {
                headerList = curl_slist_append(headerList, "Content-Type: application/json");
            } else if (spec.bodyKind == api::BodyKind::FormData) {
                mime = curl_mime_init(easy);
                for (const auto& field : spec.bodyFields) {
                    if (!field.enabled || field.key.empty()) continue;
                    curl_mimepart* part = curl_mime_addpart(mime);
                    curl_mime_name(part, field.key.c_str());
                    curl_mime_data(part, field.value.c_str(), CURL_ZERO_TERMINATED);
                }
                curl_easy_setopt(easy, CURLOPT_MIMEPOST, mime);
            } else if (spec.bodyKind == api::BodyKind::FormUrlEncoded) {
                headerList = curl_slist_append(headerList, "Content-Type: application/x-www-form-urlencoded");
            } else if (spec.bodyKind == api::BodyKind::Xml) {
                headerList = curl_slist_append(headerList, "Content-Type: application/xml");
            }
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headerList);
        }

        const CURLcode code = curl_easy_perform(easy);

        if (code == CURLE_OK) {
            result.ok = true;
            double total = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
            curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &total);
            result.totalMs = total * 1000.0;
            curl_off_t size = 0;
            curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &size);
            result.sizeBytes = static_cast<std::int64_t>(size);
        } else if (code == CURLE_ABORTED_BY_CALLBACK && cancelled_.load()) {
            result.error = "已取消";
        } else {
            result.error = curl_easy_strerror(code);
        }

        if (mime) curl_mime_free(mime);
        curl_slist_free_all(headerList);
        curl_easy_cleanup(easy);
        finish(std::move(result), generation);
    }

    void finish(api::ResponseView&& result, std::uint64_t generation) {
        {
            std::lock_guard lock(resultMutex_);
            // 传输期正文/头部累积在进度槽（供 takeProgress 增量呈现），完成后
            // 并入最终结果并清空进度槽，避免 UI 在结果投递前后读到回退快照。
            result.body = std::move(progress_.body);
            result.headers = std::move(progress_.headers);
            if (result.ok && transferTruncated_)
                result.body += "\n… [truncated at 32 MiB]";
            progress_ = api::ResponseView{};
            progressDirty_ = false;
            // 已被新请求取代的结果直接丢弃，不投递（否则会冒充新请求的结果）。
            if (generation == generation_.load()) {
                pending_ = std::move(result);
                resultReady_ = true;
            }
        }
        busy_.store(false);
        core::platform::requestUiUpdate();  // 唤醒 UI 一帧取结果
    }

    std::thread worker_;  // 常驻工作线程（构造即起，析构 join）
    mutable std::mutex queueMutex_;  // busy() 是 const：锁可变
    std::condition_variable queueCv_;
    std::optional<api::RequestSpec> queued_;  // 只保留最新一条待跑规格
    bool quit_ = false;
    std::atomic<std::uint64_t> generation_{0};  // 每次 send 递增；结果按代际投递
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> isEventStream_{false};  // 本次传输是 SSE（已关闭总超时）
    bool transferTruncated_ = false;  // 仅工作线程读写
    std::mutex resultMutex_;
    api::ResponseView pending_;
    bool resultReady_ = false;
    api::ResponseView progress_;  // 在途传输的累积快照（resultMutex_ 保护）
    bool progressDirty_ = false;
};

} // namespace

std::unique_ptr<api::ApiEngine> makeCurlEngine() {
    return std::make_unique<CurlEngine>();
}
