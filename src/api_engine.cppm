// api_engine.cppm — 引擎抽象接口与共享数据类型：
//   ApiEngine  —— 单次 API 请求（curl 实现，src/curl_engine.cpp）
//   LoadEngine —— 压测（k6 外部进程实现，引擎对象由领域 store 持有）
// WebSocket / TCP 不走引擎抽象、由 UI 层协程直接驱动：
// WS/TCP 会话在 src/ui/{ws,tcp}_session.*（页面 TaskScope 协程持有）。
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

export struct BodyContent {
    std::string text;
    std::vector<KeyValue> fields;
};

export struct RequestSpec {
    std::string method = "GET";
    std::string url;
    std::vector<KeyValue> params;    // query 参数（拼进 URL）
    std::vector<KeyValue> headers;
    std::vector<KeyValue> cookies;
    BodyKind bodyKind = BodyKind::None;
    std::string body;                // 当前 body 的文本兼容表示
    std::vector<KeyValue> bodyFields; // 当前 form body 的结构化字段
    bool followRedirects = true;
    bool allowJsonComments = true;
    int timeoutSec = 30;
    // HTTP 代理（如 http://127.0.0.1:7890 / socks5://host:port）；空 = 直连。
    // 由领域 store 在 finalizeSpec 统一注入全局设置（settings.ini request_proxy）。
    std::string proxy;
};

export struct ResponseView {
    bool ok = false;                 // 传输层是否成功（非 HTTP 状态语义）
    std::string error;               // 传输层错误（DNS / 连接 / TLS / 超时…）
    long status = 0;                 // HTTP 状态码
    std::vector<KeyValue> headers;   // 响应头（保持到达顺序）
    std::string body;
    std::int64_t sizeBytes = 0;
    double totalMs = 0;
};

// ---- 单次请求 ----

// 单次请求引擎契约。实现内部自管理后台线程；send() 立即返回，结果经
// takeResponse() 由 UI 线程取走（引擎完成时负责 requestUiUpdate 唤醒）。
export class ApiEngine {
public:
    virtual ~ApiEngine() = default;

    // 发起一次请求（异步）。busy 时再调视为替换：取消旧请求、发起新请求。
    virtual void send(const RequestSpec& spec) = 0;
    // 协作式取消当前请求（不保证立即生效，阻塞中的系统调用不可强杀）。
    // 取消后结果不投递：丢弃排队中的请求、在途结果按代际作废。
    virtual void cancel() = 0;
    // 是否有请求在途。
    virtual bool busy() const = 0;
    // UI 线程轮询：在途传输的响应头到达或正文有新增时，把当前累积快照拷贝到
    // out 并返回 true（快照按代际隔离，finish 后进度槽清空）。SSE
    // （text/event-stream）等流式响应经本接口增量呈现；结果投递语义不变。
    virtual bool takeProgress(ResponseView& out) = 0;
    // UI 线程轮询：有新完成的结果则取出并返回 true（一个结果只取一次）。
    virtual bool takeResponse(ResponseView& out) = 0;
};

// ---- 压测 ----

export struct LoadOptions {
    int vus = 10;
    std::string duration = "30s";    // k6 duration 语法（"30s" / "1m" / "2m30s"）
    int timeoutSec = 30;             // 单请求超时（写进脚本）
    // 用户自定义 k6 脚本全文（压测页脚本编辑器）：空 = 引擎按 spec 自动生成；
    // 非空 = 原样运行该脚本（vus/duration/timeoutSec 不再注入，以脚本里的 options 为准）。
    std::string script;
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
