// draft.h — 请求草稿的共享编辑类型（UI 侧）。hcg codegen 只扫 .cpp，本头只放
// 纯数据类型与无模块依赖的帮助函数；依赖模块类型（api::/db::）的转换与
// composable 一律留在 .cpp（request_page.cpp 等）。
// 持久化映射：DraftFromSaved（db→草稿）与"保存"按钮（草稿→db）在
// request_page.cpp；测试用例页/Mock 页只读写草稿字段。
#pragma once

#include <huxerui/huxerui.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace apitab::ui {

inline constexpr std::array<std::string_view, 20> kMethodNames{
    // 经典 7 个 + 近年新注册/常用扩展（QUERY 为 HTTPBIS 新方法草案；
    // PURGE 常见于 CDN；PROPFIND 起为 WebDAV 一族）。curl 引擎走
    // CURLOPT_CUSTOMREQUEST、k6 走 http.request，任意方法名都合法。
    "GET",     "POST",   "PUT",    "PATCH",   "DELETE", "HEAD", "OPTIONS",
    "CONNECT", "TRACE",  "QUERY",  "PURGE",   "PROPFIND", "PROPPATCH",
    "MKCOL",   "COPY",   "MOVE",   "LOCK",    "UNLOCK", "REPORT", "SEARCH"};

// 下标与 api::BodyKind 一一对应（None=0 … GraphQL=6）。
inline constexpr std::array<std::string_view, 7> kBodyTypeNames{
    "无", "JSON", "Text", "Form URL-Encoded", "Form-Data", "XML", "GraphQL"};

// 受控 KV 行：TextField 保留完整 TextEditingValue。type/remark 对应
// api::KeyValue 的同名字段（类型说明 / 备注，仅记录与展示，不参与发送逻辑）。
// type 为固定枚举值（string/number/boolean），由 KvTypeSelect 下拉选择或
// 按值文本自动推断（InferKvType，定义在 request_page.cpp）。
struct KvRow {
    huxerui::TextEditingValue key;
    huxerui::TextEditingValue value;
    huxerui::TextEditingValue type;
    huxerui::TextEditingValue remark;
    bool enabled = true;

    bool operator==(const KvRow&) const = default;
};

// KV 类型列的固定选项（下拉值；仅记录与展示，不参与发送逻辑）。
inline constexpr std::array<std::string_view, 3> kKvTypeNames{"string", "number", "boolean"};

// 测试用例草稿：一组对响应的断言（编辑器子页"测试用例"的数据）。
// 数字字段用文本承载（受控 TextField 语义），空串 = 不校验该项；
// 落库换算（→ db::RequestTestCase）在 request_page.cpp 的保存/载入路径。
struct TestCaseDraft {
    huxerui::TextEditingValue name;         // 用例名
    bool enabled = true;
    huxerui::TextEditingValue expectStatus; // 期望状态码，空 = 不校验
    huxerui::TextEditingValue maxMs;        // 耗时上限（毫秒），空 = 不校验
    // JSON 断言：key = 路径（点分 + 下标，如 data.items[0].id），
    // value = 期望字符串；type/remark 不使用。enabled 控制单条生效。
    std::vector<KvRow> asserts;

    bool operator==(const TestCaseDraft&) const = default;
};

// Mock 草稿：enabled 时调试页"发送"不走真实网络，直接按此定义返回模拟响应。
// status/delayMs 文本承载（空/非法按默认值），落库换算同上。
struct MockDraft {
    bool enabled = false;
    huxerui::TextEditingValue status{huxerui::TextEditingValue{std::string{"200"}}};
    std::vector<KvRow> headers; // 模拟响应头（key/value 用，type/remark 不用）
    huxerui::TextEditingValue body;
    huxerui::TextEditingValue delayMs{huxerui::TextEditingValue{std::string{"0"}}};

    bool operator==(const MockDraft&) const = default;
};

// 草稿稳定标识：未保存草稿的 savedId 恒为 0，标签拖拽排序与 .Key 需要与
// 下标无关的稳定 id（每份草稿创建时取一个递增 uid，拷贝沿用）。
inline std::uint64_t NextDraftUid() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// 请求草稿：一个内部标签页的完整编辑状态（savedId = 0 表示未保存的新请求）。
struct RequestDraft {
    std::uint64_t uid = NextDraftUid();
    std::int64_t savedId = 0;
    int kind = 0; // 0=HTTP 1=WebSocket 2=TCP（对应 api::RequestKind::Http/WebSocket/Tcp）
    huxerui::TextEditingValue name; // 标签名 / 保存名
    std::size_t methodIndex = 0;
    huxerui::TextEditingValue url;
    std::vector<KvRow> params;
    std::vector<KvRow> headers;
    std::vector<KvRow> cookies;
    std::size_t bodyKindIndex = 0; // 下标 = api::BodyKind 值
    // 各 body 类型独立一份文本（下标 = api::BodyKind 值），切换类型互不影响；
    // 文本类（JSON/Text/XML/GraphQL）各有独立编辑框，form 类只用 bodyFields。
    std::array<huxerui::TextEditingValue, 7> bodies;
    std::vector<KvRow> bodyFields; // Form 类 body 的字段
    // 编辑器子页数据（调试/文档/测试用例/Mock 中的后两者）。
    std::vector<TestCaseDraft> cases;
    MockDraft mock;

    bool operator==(const RequestDraft&) const = default;
};

inline std::string DraftDisplayName(const RequestDraft& draft) {
    return draft.name.text.empty() ? "未命名" : draft.name.text;
}

// 标签/列表徽标：HTTP 显示方法名，WS/TCP 显示类型缩写。
inline std::string DraftKindBadge(const RequestDraft& draft) {
    switch (draft.kind) {
        case 1: return "WS";
        case 2: return "TCP";
        default: return std::string{kMethodNames.at(draft.methodIndex)};
    }
}

// 草稿表受控写回：拷贝 drafts、按下标改本项、回写 State（越界静默忽略）。
template <class F>
inline void MutateDraft(huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index,
                        F&& fn) {
    std::vector<RequestDraft> copy = drafts.Get();
    if (index < copy.size()) {
        fn(copy[index]);
        drafts = copy;
    }
}

} // namespace apitab::ui
