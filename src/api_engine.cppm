// api_engine.cppm — 引擎抽象接口。UI 与 store 只面向这里的数据类型与两个抽象：
//   ApiEngine  —— 单次 API 请求（curl 实现）
//   LoadEngine —— 压测（k6 外部进程实现）
// 与 tinynext 的 download_engine.cppm 同位：引擎对象由领域 store 持有（自保留），
// UI 从不直接碰引擎指针。
export module apitab.api_engine;

import std;

namespace api {

// ---- 通用键值对（headers / query params 共用）----

export struct KeyValue {
    std::string key;
    std::string value;
    bool enabled = true;
    std::string type;
    std::string remark;
};

export enum class BodyKind {
    None = 0,
    Json = 1,
    Text = 2,
    FormUrlEncoded = 3,
    FormData = 4,
    Xml = 5,
    GraphQL = 6
};
export enum class RequestKind { Http = 0, WebSocket = 1, Tcp = 2 };
export enum class TcpState { Disconnected, Resolving, Connecting, Handshaking, Connected, Failed };
export enum class TcpEventKind { Connecting, Connected, Sent, Received, Disconnected, Error };
export enum class TcpPayloadFormat { Text, Hex };
export enum class WebSocketState { Disconnected, Connecting, Connected, Failed };
export enum class WebSocketEventKind { Open, Text, Binary, Close, Error };

export struct TcpSpec {
    std::string url;
    int connectTimeoutSec = 15;
    int readIdleTimeoutSec = 0;
    int writeTimeoutSec = 10;
};

export struct TcpEvent {
    TcpEventKind kind = TcpEventKind::Error;
    std::vector<std::uint8_t> payload;
    std::string detail;
    std::size_t wireBytes = 0;
};

export struct WebSocketSpec {
    std::string url;
    std::vector<KeyValue> headers;
    std::string subprotocol;
    int handshakeTimeoutSec = 15;
};

export struct WebSocketEvent {
    WebSocketEventKind kind = WebSocketEventKind::Error;
    std::string payload;
    std::string detail;
    std::size_t wireBytes = 0;
    int closeCode = 0;
};

export class TcpEngine {
public:
    virtual ~TcpEngine() = default;
    virtual std::string connect(const TcpSpec& spec) = 0;
    virtual void disconnect() = 0;
    virtual std::string send(const std::vector<std::uint8_t>& bytes) = 0;
    virtual TcpState state() const = 0;
    virtual std::vector<TcpEvent> drainEvents() = 0;
};


export struct RequestSpec {
    std::string method = "GET";
    std::string url;
    std::vector<KeyValue> params;    // query 参数（拼进 URL）
    std::vector<KeyValue> headers;
    std::vector<KeyValue> cookies;
    BodyKind bodyKind = BodyKind::None;
    std::string body;
    bool followRedirects = true;
    bool allowJsonComments = true;
    int timeoutSec = 30;
};

export struct ResponseView {
    bool ok = false;                 // curl 传输层是否成功（非 HTTP 状态语义）
    std::string error;               // 传输层错误（DNS / 连接 / TLS / 超时…）
    long status = 0;                 // HTTP 状态码
    std::vector<KeyValue> headers;   // 响应头（保持到达顺序）
    std::string body;
    std::int64_t sizeBytes = 0;
    double totalMs = 0;
    double dnsMs = 0;
    double connectMs = 0;
    double tlsMs = 0;
};

// ---- 压测 ----

export struct LoadOptions {
    int vus = 10;
    std::string duration = "30s";    // k6 duration 语法（"30s" / "1m" / "2m30s"）
    int timeoutSec = 30;             // 单请求超时（写进脚本）
};

// k6 结束后的汇总指标（从脚本 handleSummary 打印的 JSON 行解析）。
export struct LoadSummary {
    bool ok = false;                 // 是否拿到完整汇总（被 stop / k6 崩溃 = false）
    std::string error;
    std::int64_t requests = 0;       // http_reqs.count
    double rps = 0;                  // http_reqs.rate
    double avgMs = 0, minMs = 0, maxMs = 0;
    double p50Ms = 0, p90Ms = 0, p95Ms = 0, p99Ms = 0;
    double failRate = 0;             // http_req_failed.rate（0..1）
    double durationSec = 0;          // 实际运行时长（墙钟）
};

// 单次请求引擎契约。实现内部自管理后台线程；send() 立即返回，结果经
// takeResponse() 由 UI 线程取走（引擎完成时负责 requestUiUpdate 唤醒）。
export class ApiEngine {
public:
    virtual ~ApiEngine() = default;

    // 发起一次请求（异步）。busy 时再调视为替换：取消旧请求、发起新请求。
    virtual void send(const RequestSpec& spec) = 0;
    // 协作式取消当前请求（不保证立即生效，阻塞中的系统调用不可强杀）。
    virtual void cancel() = 0;
    // 是否有请求在途。
    virtual bool busy() const = 0;
    // UI 线程轮询：有新完成的结果则取出并返回 true（一个结果只取一次）。
    virtual bool takeResponse(ResponseView& out) = 0;
};

export class WebSocketEngine {
public:
    virtual ~WebSocketEngine() = default;
    virtual std::string connect(const WebSocketSpec& spec) = 0;
    virtual void disconnect() = 0;
    virtual std::string sendText(const std::string& text, bool binary) = 0;
    virtual WebSocketState state() const = 0;
    virtual std::vector<WebSocketEvent> drainEvents() = 0;
};

// 压测引擎契约。实现为外部进程（k6）：start 拉起、stop 杀掉、输出流式回读。
export class LoadEngine {
public:
    virtual ~LoadEngine() = default;

    // k6 二进制是否可用（engines/ 或 PATH 解析成功）。
    virtual bool available() const = 0;
    virtual std::string binaryPath() const = 0;

    // 由请求模板生成 k6 脚本并拉起进程。running 时再调视为先 stop 再 start。
    virtual void start(const RequestSpec& spec, const LoadOptions& opts) = 0;
    // 杀掉压测进程（立即，非协作）。
    virtual void stop() = 0;
    virtual bool running() const = 0;

    // UI 线程取走新增的输出行（k6 stdout/stderr，\r 进度行已拆成独立行）。
    virtual std::vector<std::string> drainOutput() = 0;
    // UI 线程轮询：压测结束且汇总未取走则取出（ok=false 表示异常结束）。
    virtual bool takeSummary(LoadSummary& out) = 0;
};

} // namespace api
