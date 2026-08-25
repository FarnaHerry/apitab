// curl_engine.cpp — apitab.curl_engine 实现单元。
//
// 线程模型（继承 tinynext 引擎纪律）：send() 在 UI 线程调用、立即返回；实际
// 传输在工作线程里同步跑（curl_easy_perform 阻塞），完成后写互斥保护的结果槽并
// requestUiUpdate() 唤醒 UI 一帧。UI 线程 compose 时 takeResponse() 取走。
// 取消是协作式的：CURLOPT_XFERINFOFUNCTION 每帧查原子标志，置位即中断传输
// （curl 返回 CURLE_ABORTED_BY_CALLBACK）。
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
    CurlEngine() { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlEngine() override { cancel(); joinWorker(); }

    void send(const api::RequestSpec& spec) override {
        cancel();       // 打断在途请求
        joinWorker();   // 等工作线程退出（保证同一线程串行用结果槽）
        cancelled_.store(false);
        busy_.store(true);
        worker_ = std::thread([this, spec] { run(spec); });
    }

    void cancel() override {
        if (busy_.load()) cancelled_.store(true);
    }

    bool busy() const override { return busy_.load(); }

    bool takeResponse(api::ResponseView& out) override {
        std::lock_guard lock(resultMutex_);
        if (!resultReady_) return false;
        out = std::move(pending_);
        pending_ = api::ResponseView{};
        resultReady_ = false;
        return true;
    }

private:
    void joinWorker() {
        if (worker_.joinable()) worker_.join();
    }

    static int onProgress(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        const auto* self = static_cast<const CurlEngine*>(clientp);
        return self->cancelled_.load() ? 1 : 0;  // 非 0 → CURLE_ABORTED_BY_CALLBACK
    }

    void run(const api::RequestSpec& spec) {
        api::ResponseView result;

        CURL* easy = curl_easy_init();
        if (!easy) {
            result.error = "curl_easy_init failed";
            finish(std::move(result));
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
        if (spec.bodyKind != api::BodyKind::None) {
            std::string body = spec.body;
            if (spec.bodyKind == api::BodyKind::Json && spec.allowJsonComments) {
                body = stripJsonComments(body);
            }
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(body.size()));
            if (spec.bodyKind == api::BodyKind::Json || spec.bodyKind == api::BodyKind::GraphQL) {
                headerList = curl_slist_append(headerList, "Content-Type: application/json");
            } else if (spec.bodyKind == api::BodyKind::FormData) {
                const auto fields = parseFormData(body);
                mime = curl_mime_init(easy);
                for (const auto& [name, value] : fields) {
                    curl_mimepart* part = curl_mime_addpart(mime);
                    curl_mime_name(part, name.c_str());
                    curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
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
        finish(std::move(result));
    }

    void finish(api::ResponseView&& result) {
        {
            std::lock_guard lock(resultMutex_);
            pending_ = std::move(result);
            resultReady_ = true;
        }
        busy_.store(false);
        core::platform::requestUiUpdate();  // 唤醒 UI 一帧取结果
    }

    std::thread worker_;
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
