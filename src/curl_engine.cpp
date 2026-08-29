// curl_engine.cpp — apitab.curl_engine 实现单元。
//
// 线程模型：常驻工作线程 + 条件变量队列。send() 在 UI 线程调用、纯入队立即
// 返回——绝不在 UI 线程 join 工作线程（旧实现 send 时 join 上一条传输，端点
// 挂起时整条 UI 线程被拖死）。工作线程串行消费队列：新请求到来时协作打断在途
// 传输（CURLOPT_XFERINFOFUNCTION 每帧查原子标志，curl 返回
// CURLE_ABORTED_BY_CALLBACK），被取代请求的结果按代际丢弃，不会冒充新请求的
// 结果。完成后写互斥保护的结果槽并 requestUiUpdate() 唤醒 UI 一帧，UI 轮询
// takeResponse() 取走。
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

struct WriteCtx {
    std::string* body;
    bool truncated = false;
};

size_t onBodyWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    auto* ctx = static_cast<WriteCtx*>(userdata);
    const size_t room = kMaxBodyBytes - std::min(ctx->body->size(), kMaxBodyBytes);
    const size_t take = std::min(n, room);
    ctx->body->append(ptr, take);
    if (take < n) ctx->truncated = true;
    return n;  // 吞掉全部（不截断传输本身，只截断留存）
}

struct HeaderCtx {
    std::vector<api::KeyValue>* headers;
};

size_t onHeaderLine(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t n = size * nmemb;
    auto* ctx = static_cast<HeaderCtx*>(userdata);
    std::string line(ptr, n);
    // 去掉行尾 \r\n；跳过状态行（HTTP/...）与空行。
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    const auto colon = line.find(':');
    if (colon != std::string::npos && !line.starts_with("HTTP/")) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        // value 前导空格
        const auto pos = value.find_first_not_of(' ');
        if (pos != std::string::npos) value.erase(0, pos);
        ctx->headers->push_back({std::move(key), std::move(value), true});
    }
    return n;
}

std::string stripJsonComments(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = 0; i < input.size();) {
        const char c = input[i];
        if (inString) {
            output.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            ++i;
            continue;
        }
        if (c == '"') {
            inString = true;
            output.push_back(c);
            ++i;
        } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
            i += 2;
            while (i < input.size() && input[i] != '\n') ++i;
        } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
            i += 2;
            while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
            if (i + 1 < input.size()) i += 2;
        } else {
            output.push_back(c);
            ++i;
        }
    }
    return output;
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
    CurlEngine() : worker_([this] { workerLoop(); }) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
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
        queueCv_.notify_one();
    }

    void cancel() override {
        std::lock_guard lock(queueMutex_);
        cancelled_.store(true);
    }

    bool busy() const override {
        if (busy_.load()) return true;
        std::lock_guard lock(queueMutex_);
        return queued_.has_value();
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
                busy_.store(true);
            }
            {   // 丢弃上一请求遗留的结果槽（已被新请求取代）
                std::lock_guard rlock(resultMutex_);
                pending_ = api::ResponseView{};
                resultReady_ = false;
            }
            run(spec, generation);
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

        WriteCtx bodyCtx{&result.body};
        HeaderCtx headerCtx{&result.headers};
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
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &bodyCtx);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &onHeaderLine);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &headerCtx);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, static_cast<long>(spec.timeoutSec));
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
            if (spec.bodyKind == api::BodyKind::Json && spec.allowJsonComments) {
                body = stripJsonComments(body);
            }
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
            double total = 0, dns = 0, connect = 0, tls = 0;
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
            curl_easy_getinfo(easy, CURLINFO_TOTAL_TIME, &total);
            curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME, &dns);
            curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connect);
            curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME, &tls);
            result.totalMs = total * 1000.0;
            result.dnsMs = dns * 1000.0;
            result.connectMs = connect * 1000.0;
            result.tlsMs = tls * 1000.0;
            curl_off_t size = 0;
            curl_easy_getinfo(easy, CURLINFO_SIZE_DOWNLOAD_T, &size);
            result.sizeBytes = static_cast<std::int64_t>(size);
            if (bodyCtx.truncated) {
                result.body += "\n… [truncated at 32 MiB]";
            }
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
    std::mutex resultMutex_;
    api::ResponseView pending_;
    bool resultReady_ = false;
};

} // namespace

std::unique_ptr<api::ApiEngine> makeCurlEngine() {
    return std::make_unique<CurlEngine>();
}
