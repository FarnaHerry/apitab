// request_page.cpp — 请求工作区：左岛 = 当前项目的请求列表；右岛 = 编辑区，
// 右岛顶部为内部标签页（每个标签一个请求草稿，可开可关）。
// 发送经 TaskScope 协程轮询 curl 引擎；保存落到当前项目集合并刷新左岛列表。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui.h"
#include "app_resources.h"

import apitab.api_engine;
import apitab.db;
import apitab.preferences;
import apitab.store.requests;

namespace apitab::ui {

namespace {
constexpr std::array<std::string_view, 7> kMethodNames{
    "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};

// 下标与 api::BodyKind 一一对应（None=0 … GraphQL=6）。
constexpr std::array<std::string_view, 7> kBodyTypeNames{
    "无", "JSON", "Text", "Form URL-Encoded", "Form-Data", "XML", "GraphQL"};

// 受控 KV 行：TextField 保留完整 TextEditingValue。
struct KvRow {
    huxerui::TextEditingValue key;
    huxerui::TextEditingValue value;
    bool enabled = true;

    bool operator==(const KvRow&) const = default;
};

api::KeyValue ToKeyValue(const KvRow& row) {
    return api::KeyValue{.key = row.key.text,
                         .value = row.value.text,
                         .enabled = row.enabled};
}

KvRow FromKeyValue(const api::KeyValue& kv) {
    return KvRow{.key = huxerui::TextEditingValue{kv.key},
                 .value = huxerui::TextEditingValue{kv.value},
                 .enabled = kv.enabled};
}

// KV 编辑表（回调风格）：rows 为快照，任何增删改经 onChanged 回写新 vector。
// 之所以不用 State 参数：行数据现在寄宿在请求草稿（RequestDraft）内部。
[[huxerui::composable]] huxerui::View KvTable(
    std::vector<KvRow> rows, const huxerui::ThemeSpec& theme, std::string keyLabel,
    std::string valueLabel, std::function<void(std::vector<KvRow>)> onChanged) {
    auto tasks = huxerui::UseTaskScope();
    std::vector<huxerui::View> children{huxerui::Text(keyLabel, huxerui::TextRole::Label)};

    // 自动追加语义：末尾恒渲染一个虚拟空行；对它写入即物化为真实行，
    // 重组后尾部再出现新的虚拟空行。✕ 只给真实行（虚拟行留占位保持行高）。
    for (std::size_t i = 0; i <= rows.size(); ++i) {
        const bool phantom = i == rows.size();
        const KvRow row = phantom ? KvRow{} : rows[i];
        // 行写入：i 越界（虚拟行）时物化新行，否则改写原行。
        // 虚拟行只在真正输入了文本时才物化——聚焦/移动光标触发的 OnChanged
        // （text 为空、仅选区变化）不追加新行。
        auto applyRow = [rows, onChanged](std::size_t i, KvRow updated) {
            std::vector<KvRow> copy = rows;
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.key.text.empty() && updated.value.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            onChanged(std::move(copy));
        };
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(row.enabled).OnChanged([row, i, applyRow](bool checked) {
                    KvRow updated = row;
                    updated.enabled = checked;
                    applyRow(i, std::move(updated));
                }),
                huxerui::TextField(row.key)
                    .Label(keyLabel)
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.key = value;
                        applyRow(i, std::move(updated));
                    }),
                huxerui::TextField(row.value)
                    .Label(valueLabel)
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.value = value;
                        applyRow(i, std::move(updated));
                    }),
                phantom
                    ? huxerui::View{huxerui::Text("", huxerui::TextRole::Label)
                                        .With(huxerui::Padding(4.0F))}
                    : huxerui::View{huxerui::Button("✕").OnClick([tasks, rows, i, onChanged] {
                        // 删除会移除本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<KvRow> copy = rows;
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            onChanged(std::move(copy));
                        });
                    })},
            }
                .With(huxerui::Spacing(theme.spacing.small)));
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 请求草稿：一个内部标签页的完整编辑状态（savedId = 0 表示未保存的新请求）。
struct RequestDraft {
    std::int64_t savedId = 0;
    int kind = 0; // 0=HTTP 1=WebSocket 2=TCP（对应 api::RequestKind::Http/WebSocket/Tcp）
    huxerui::TextEditingValue name; // 标签名 / 保存名
    std::size_t methodIndex = 0;
    huxerui::TextEditingValue url;
    std::vector<KvRow> params;
    std::vector<KvRow> headers;
    std::vector<KvRow> cookies;
    std::size_t bodyKindIndex = 0; // 下标 = api::BodyKind 值
    huxerui::TextEditingValue body;
    std::vector<KvRow> bodyFields; // Form 类 body 的字段

    bool operator==(const RequestDraft&) const = default;
};

std::string DraftDisplayName(const RequestDraft& draft) {
    return draft.name.text.empty() ? "未命名" : draft.name.text;
}

// 标签/列表徽标：HTTP 显示方法名，WS/TCP 显示类型缩写。
std::string DraftKindBadge(const RequestDraft& draft) {
    switch (draft.kind) {
        case 1: return "WS";
        case 2: return "TCP";
        default: return std::string{kMethodNames.at(draft.methodIndex)};
    }
}

RequestDraft DraftFromSaved(const db::SavedRequest& saved) {
    RequestDraft draft;
    draft.savedId = saved.id;
    draft.kind = static_cast<int>(saved.kind);
    if (draft.kind < 0 || draft.kind > 2) draft.kind = 0; // 防御：未知类型按 HTTP 打开
    draft.name = huxerui::TextEditingValue{saved.name};
    for (std::size_t i = 0; i < kMethodNames.size(); ++i) {
        if (kMethodNames[i] == saved.method) draft.methodIndex = i;
    }
    draft.url = huxerui::TextEditingValue{saved.url};
    for (const api::KeyValue& kv : saved.params) draft.params.push_back(FromKeyValue(kv));
    for (const api::KeyValue& kv : saved.headers) draft.headers.push_back(FromKeyValue(kv));
    for (const api::KeyValue& kv : saved.cookies) draft.cookies.push_back(FromKeyValue(kv));
    draft.bodyKindIndex = static_cast<std::size_t>(saved.bodyKind);
    if (draft.bodyKindIndex >= kBodyTypeNames.size()) draft.bodyKindIndex = 0;
    // 文本优先取按类型归档的 bodyContents，回落到兼容字段 body。
    std::string text = saved.body;
    std::vector<api::KeyValue> fields;
    if (draft.bodyKindIndex < saved.bodyContents.size()) {
        if (!saved.bodyContents[draft.bodyKindIndex].text.empty())
            text = saved.bodyContents[draft.bodyKindIndex].text;
        fields = saved.bodyContents[draft.bodyKindIndex].fields;
    }
    draft.body = huxerui::TextEditingValue{text};
    for (const api::KeyValue& kv : fields) draft.bodyFields.push_back(FromKeyValue(kv));
    return draft;
}

// 从草稿组装请求规格：params 拼接与全局 Cookie 由领域 store 统一处理。
api::RequestSpec SpecFromDraft(const RequestDraft& draft) {
    api::RequestSpec spec;
    spec.method = std::string{kMethodNames.at(draft.methodIndex)};
    spec.url = draft.url.text;
    for (const KvRow& row : draft.params)
        if (row.enabled && !row.key.text.empty()) spec.params.push_back(ToKeyValue(row));
    for (const KvRow& row : draft.headers)
        if (row.enabled && !row.key.text.empty()) spec.headers.push_back(ToKeyValue(row));
    for (const KvRow& row : draft.cookies)
        if (row.enabled && !row.key.text.empty()) spec.cookies.push_back(ToKeyValue(row));
    spec.bodyKind = static_cast<api::BodyKind>(draft.bodyKindIndex);
    switch (spec.bodyKind) {
        case api::BodyKind::None:
            break;
        case api::BodyKind::FormUrlEncoded:
        case api::BodyKind::FormData:
            for (const KvRow& row : draft.bodyFields)
                if (row.enabled && !row.key.text.empty())
                    spec.bodyFields.push_back(ToKeyValue(row));
            break;
        default: // Json/Text/Xml/GraphQL：文本体
            spec.body = draft.body.text;
            break;
    }
    return spec;
}

// 回写草稿中某个字段的便捷方式：copy → mutate → set。
template <typename F>
void MutateDraft(huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index, F&& fn) {
    std::vector<RequestDraft> copy = drafts.Get();
    if (index < copy.size()) {
        fn(copy[index]);
        drafts = copy;
    }
}

// 响应区：独立重组作用域 —— responseTab/responseBody/responseHeaders 的变化只
// 重组此区域，不扩散到整个编辑器（KV 表不受影响）。
[[huxerui::composable]] huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                                                   huxerui::State<std::vector<std::string>> responseHeaders,
                                                   const huxerui::ThemeSpec& theme) {
    auto responseTab = huxerui::UseState<std::size_t>(0);

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(13.0F),
                                  .foreground = theme.colors.on_surface};
    std::vector<huxerui::View> children{
        huxerui::SegmentedButton({"Body", "Headers"}, responseTab)
            .OnChanged([responseTab](std::size_t index) { responseTab = index; })};
    if (responseTab.Get() == 0) {
        children.push_back(huxerui::SelectionArea{
            huxerui::Text(responseBody.Get(), huxerui::TextRole::Body).Style(mono)});
    } else {
        const std::vector<std::string> lines = responseHeaders.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        children.push_back(
            rows.empty()
                ? huxerui::View{huxerui::Text("（无响应头）", huxerui::TextRole::Body)}
                : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                           .With(huxerui::Spacing(2.0F))}});
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 右岛顶部内部标签条：每个打开的草稿一个标签（点击切换 / ✕ 关闭），末尾 "＋" 新建。
[[huxerui::composable]] huxerui::View RequestTabStrip(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    const auto chipFont = huxerui::Font::System(12.0F);
    const auto badgeFont =
        huxerui::Font::Monospace(10.0F).WithWeight(huxerui::FontWeight::SemiBold);

    const std::vector<RequestDraft> snapshot = drafts.Get();
    std::vector<huxerui::View> chips;
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const bool active = i == activeTab.Get();
        const huxerui::Color fill =
            active ? theme.colors.surface_container_highest : theme.colors.surface_container;
        const huxerui::Color foreground =
            active ? theme.colors.on_surface : theme.colors.on_surface_variant;
        chips.push_back(
            huxerui::Row {
                // 类型徽标：HTTP 显示方法名，WS/TCP 显示类型缩写。
                huxerui::Text(DraftKindBadge(snapshot[i]), huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = badgeFont,
                                              .foreground = theme.colors.primary})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(2.0F, 2.0F)))
                    .OnClick([drafts, activeTab, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) activeTab = i;
                    }),
                huxerui::Text(DraftDisplayName(snapshot[i]), huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont, .foreground = foreground})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)))
                    .OnClick([drafts, activeTab, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) activeTab = i;
                    }),
                huxerui::Text("✕", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont, .foreground = foreground})
                    .With(huxerui::Padding(4.0F))
                    .OnClick([tasks, drafts, activeTab, i] {
                        // 关闭会卸载本 ✕ 所在标签：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<RequestDraft> copy = drafts.Get();
                            if (i >= copy.size()) co_return;
                            copy.erase(copy.begin() + static_cast<long>(i));
                            drafts = copy;
                            if (!copy.empty() && activeTab.Get() >= copy.size())
                                activeTab = copy.size() - 1;
                        });
                    })
                    .On<huxerui::ViewEvents::PointerMove>([](const huxerui::PointerEvent&) {}),
            }
                .With(huxerui::Spacing(0.0F), huxerui::Background(fill),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                      huxerui::Frame{.height = 26.0F, .max_width = 160.0F},
                      huxerui::ClipChildren())
                .Key(snapshot[i].savedId != 0 ? snapshot[i].savedId
                                              : -static_cast<std::int64_t>(i + 1)));
    }
    // 末尾 "＋"：新建草稿标签（不卸载任何节点，同步写即可）。
    chips.push_back(huxerui::Text("+", huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{.font = chipFont,
                                                  .foreground = theme.colors.on_surface_variant})
                        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
                              huxerui::Background(theme.colors.surface_container),
                              huxerui::CornerRadius(theme.shapes.small))
                        .OnClick([drafts, activeTab] {
                            std::vector<RequestDraft> copy = drafts.Get();
                            copy.push_back(RequestDraft{});
                            drafts = copy;
                            activeTab = copy.size() - 1;
                        }));

    return huxerui::Row(std::move(chips))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::ClipChildren());
}

// 右岛编辑区：方法/URL/名称/发送/保存 一行 + Params/Headers/Cookies/Body 分区 + 响应区。
[[huxerui::composable]] huxerui::View RequestEditor(
    huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index,
    huxerui::State<int> listVersion, huxerui::State<bool> inFlight,
    huxerui::State<std::string> responseBody,
    huxerui::State<std::vector<std::string>> responseHeaders) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto section = huxerui::UseState<std::size_t>(0); // 0=Params 1=Headers 2=Cookies 3=Body

    const std::vector<RequestDraft> all = drafts.Get();
    if (index >= all.size()) {
        return huxerui::Text("（标签页已关闭）", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant));
    }
    const RequestDraft snapshot = all[index];

    std::vector<huxerui::View> editorChildren{huxerui::SegmentedButton(
        {"Params", "Headers", "Cookies", "Body"}, section)
                                      .OnChanged([section](std::size_t i) { section = i; })};
    switch (section.Get()) {
        case 0:
            editorChildren.push_back(KvTable(
                snapshot.params, theme, "参数名", "参数值",
                [drafts, index](std::vector<KvRow> rows) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.params = std::move(rows); });
                }));
            break;
        case 1:
            editorChildren.push_back(KvTable(
                snapshot.headers, theme, "头名称", "头值",
                [drafts, index](std::vector<KvRow> rows) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.headers = std::move(rows); });
                }));
            break;
        case 2:
            editorChildren.push_back(KvTable(
                snapshot.cookies, theme, "Cookie 名", "Cookie 值",
                [drafts, index](std::vector<KvRow> rows) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.cookies = std::move(rows); });
                }));
            break;
        default: {
            // Body：先选类型（下标 = api::BodyKind），再按类型展开对应编辑器。
            std::vector<huxerui::View> bodyChildren{huxerui::SegmentedButton(
                {"无", "JSON", "Text", "Form URL-Encoded", "Form-Data", "XML", "GraphQL"},
                snapshot.bodyKindIndex)
                                                        .OnChanged([drafts, index](std::size_t kind) {
                                                            MutateDraft(drafts, index,
                                                                        [kind](RequestDraft& d) {
                                                                            d.bodyKindIndex = kind;
                                                                        });
                                                        })};
            switch (snapshot.bodyKindIndex) {
                case 0: // 无
                    bodyChildren.push_back(
                        huxerui::Text("无请求体", huxerui::TextRole::Body)
                            .With(huxerui::Foreground(theme.colors.on_surface_variant)));
                    break;
                case 3: // Form URL-Encoded
                case 4: // Form-Data
                    bodyChildren.push_back(KvTable(
                        snapshot.bodyFields, theme, "字段名", "字段值",
                        [drafts, index](std::vector<KvRow> rows) {
                            MutateDraft(drafts, index,
                                        [&](RequestDraft& d) { d.bodyFields = std::move(rows); });
                        }));
                    break;
                default: // JSON/Text/XML/GraphQL
                    bodyChildren.push_back(
                        huxerui::TextField(snapshot.body)
                            .Label("Body（" +
                                   std::string{kBodyTypeNames.at(snapshot.bodyKindIndex)} + "）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(4, 12))
                            .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                                MutateDraft(drafts, index,
                                            [&](RequestDraft& d) { d.body = value; });
                            }));
                    break;
            }
            editorChildren.push_back(huxerui::Column(std::move(bodyChildren))
                                         .With(huxerui::Spacing(theme.spacing.small),
                                               huxerui::CrossAlign(
                                                   huxerui::CrossAxisAlignment::Stretch)));
            break;
        }
    }

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(13.0F),
                                  .foreground = theme.colors.on_surface};

    return huxerui::Column {
        // 操作栏：方法下拉 + URL（占满）+ 名称 + 发送 + 保存。
        huxerui::Row {
            DropdownSelect(
                std::vector<std::string>(kMethodNames.begin(), kMethodNames.end()),
                snapshot.methodIndex,
                [drafts, index](std::size_t method) {
                    MutateDraft(drafts, index,
                                [method](RequestDraft& d) { d.methodIndex = method; });
                }),
            huxerui::TextField(snapshot.url)
                .Label("URL")
                .Placeholder("https://api.example.com/v1/resource")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                    MutateDraft(drafts, index, [&](RequestDraft& d) { d.url = value; });
                })
                .With(huxerui::Grow(1.0F)),
            huxerui::TextField(snapshot.name)
                .Label("名称")
                .Placeholder("请求名称")
                .Variant(huxerui::TextFieldVariant::Standard)
                .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                    MutateDraft(drafts, index, [&](RequestDraft& d) { d.name = value; });
                })
                .With(huxerui::Frame{.width = 140.0F}),
            huxerui::Button(inFlight.Get() ? "发送中…" : "发送").OnClick([=] {
                if (inFlight.Get()) return;
                const std::vector<RequestDraft> current = drafts.Get();
                if (index >= current.size()) return;
                api::RequestSpec spec = SpecFromDraft(current[index]);
                if (spec.url.empty()) {
                    toast.Show("URL 不能为空");
                    return;
                }
                inFlight = true;
                responseBody = "发送中…";
                responseHeaders = {};
                g_requests.send(spec, current[index].savedId);
                tasks.Launch([=]() -> huxerui::Task<void> {
                    api::ResponseView view;
                    while (!g_requests.pollResult(view)) {
                        if (!g_requests.busy()) {
                            responseBody = "请求未完成（引擎空闲）";
                            inFlight = false;
                            co_return;
                        }
                        co_await huxerui::Delay(std::chrono::duration<double>{0.05});
                    }
                    if (view.ok) {
                        responseBody = std::format("HTTP {} · {} · {} bytes\n\n{}", view.status,
                                                   view.totalMs, view.sizeBytes, view.body);
                        std::vector<std::string> lines;
                        for (const api::KeyValue& h : view.headers)
                            lines.push_back(h.key + ": " + h.value);
                        responseHeaders = lines;
                    } else {
                        responseBody = "请求失败: " + view.error;
                    }
                    inFlight = false;
                });
            }),
            huxerui::Button("保存").OnClick([=] {
                const std::vector<RequestDraft> current = drafts.Get();
                if (index >= current.size()) return;
                const RequestDraft& draft = current[index];
                if (draft.name.text.empty()) {
                    toast.Show("请先在“名称”里填写请求名");
                    return;
                }
                db::SavedRequest saved;
                saved.id = draft.savedId; // 0 = 新建；非 0 = 更新原集合项
                saved.name = draft.name.text;
                api::RequestSpec spec = SpecFromDraft(draft);
                saved.method = spec.method;
                saved.url = spec.url;
                saved.params = spec.params;
                saved.headers = spec.headers;
                saved.cookies = spec.cookies;
                saved.bodyKind = spec.bodyKind;
                saved.body = spec.body;
                // form 类 body 的结构化字段按类型落进 bodyContents，避免保存后丢失。
                const std::size_t bodyIndex = static_cast<std::size_t>(spec.bodyKind);
                if (bodyIndex < saved.bodyContents.size()) {
                    saved.bodyContents[bodyIndex].text = spec.body;
                    saved.bodyContents[bodyIndex].fields = spec.bodyFields;
                }
                if (const std::string err = g_requests.save(saved); !err.empty()) {
                    toast.Show("保存失败: " + err);
                } else {
                    toast.Show("已保存到集合");
                    // 新草稿拿到持久化 id；左岛列表刷新。
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.savedId = saved.id; });
                    listVersion = listVersion.Get() + 1;
                }
            }),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Column(std::move(editorChildren)).With(huxerui::Spacing(theme.spacing.medium)),
        // 响应区：独立重组作用域 —— responseTab/responseBody/responseHeaders 的变化只
        // 重组此区域，不扩散到整个编辑器（KV 表不受影响）。
        ResponseArea(responseBody, responseHeaders, theme),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 左岛：当前项目的请求列表（点击开标签，✕ 从集合删除）+ 顶部“＋ 新建 ▾”类型菜单。
// vertical=true 用于 Compact 视口：列表改为顶部横岛（限高、宽度撑满）。
[[huxerui::composable]] huxerui::View RequestListIsland(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<int> listVersion, std::string projectName, bool vertical) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto menu = huxerui::UseMenu();
    (void)listVersion.Get(); // 订阅列表版本：保存/删除后触发本岛重组

    const auto methodFont =
        huxerui::Font::Monospace(11.0F).WithWeight(huxerui::FontWeight::SemiBold);

    std::vector<huxerui::View> rows;
    const std::vector<db::SavedRequest>& saved = g_requests.list();
    for (const db::SavedRequest& r : saved) {
        const std::int64_t id = r.id;
        // 徽标：非 HTTP 的已保存请求显示类型缩写（防御；现存数据基本都是 HTTP）。
        const std::string badge = r.kind == api::RequestKind::WebSocket ? "WS"
                                  : r.kind == api::RequestKind::Tcp     ? "TCP"
                                                                        : r.method;
        rows.push_back(
            huxerui::Row {
                // 打开区：点击 = 打开/激活对应标签。
                huxerui::Row {
                    huxerui::Text(badge, huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{.font = methodFont,
                                                  .foreground = theme.colors.primary})
                        .With(huxerui::Frame{.min_width = 32.0F}),
                    huxerui::Text(r.name.empty() ? "（未命名）" : r.name,
                                  huxerui::TextRole::Body),
                }
                    .With(huxerui::Spacing(theme.spacing.extra_small),
                          huxerui::Grow(1.0F), huxerui::ClipChildren())
                    .OnClick([drafts, activeTab, id] {
                        // 已打开则激活，否则新建标签；左岛不卸载，同步写即可。
                        std::vector<RequestDraft> copy = drafts.Get();
                        for (std::size_t i = 0; i < copy.size(); ++i) {
                            if (copy[i].savedId == id) {
                                activeTab = i;
                                return;
                            }
                        }
                        if (const db::SavedRequest* found = g_requests.find(id)) {
                            copy.push_back(DraftFromSaved(*found));
                            drafts = copy;
                            activeTab = copy.size() - 1;
                        }
                    }),
                // 删除区：独立兄弟，点击从集合删除（并关掉对应标签）。
                huxerui::Text("✕", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = huxerui::Font::System(11.0F),
                                              .foreground = theme.colors.on_surface_variant})
                    .With(huxerui::Padding(4.0F))
                    .OnClick([tasks, drafts, activeTab, listVersion, id] {
                        // 删除会移除本行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            (void)g_requests.remove(id);
                            std::vector<RequestDraft> copy = drafts.Get();
                            for (std::size_t i = 0; i < copy.size(); ++i) {
                                if (copy[i].savedId == id) {
                                    copy.erase(copy.begin() + static_cast<long>(i));
                                    if (!copy.empty() && activeTab.Get() >= copy.size())
                                        activeTab = copy.size() - 1;
                                    break;
                                }
                            }
                            drafts = copy;
                            listVersion = listVersion.Get() + 1;
                        });
                    })
                    .On<huxerui::ViewEvents::PointerMove>([](const huxerui::PointerEvent&) {}),
            }
                .With(huxerui::Spacing(0.0F),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                      huxerui::Background(theme.colors.surface_container),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    if (saved.empty()) {
        rows.push_back(huxerui::Text("集合为空：在右侧新建请求并保存。",
                                     huxerui::TextRole::Body)
                           .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }

    // “＋ 新建 ▾”：动作菜单选择请求类型；菜单项回调在菜单层关闭后执行，
    // 不卸载被点按钮（按钮在本岛，菜单在层上），可直接写 State。
    huxerui::View island = huxerui::Column {
                               PageHeader("请求", "项目: " + std::move(projectName)),
                               huxerui::Button("＋ 新建 ▾")
                                   .OnClick([menu, drafts, activeTab] {
                                       auto pushDraft = [drafts, activeTab](int kind) {
                                           std::vector<RequestDraft> copy = drafts.Get();
                                           copy.push_back(RequestDraft{.kind = kind});
                                           drafts = copy;
                                           activeTab = copy.size() - 1;
                                       };
                                       std::vector<huxerui::MenuEntry> entries;
                                       entries.push_back(huxerui::MenuItem(
                                           "HTTP 请求", [pushDraft] { pushDraft(0); }));
                                       entries.push_back(huxerui::MenuItem(
                                           "WebSocket", [pushDraft] { pushDraft(1); }));
                                       entries.push_back(huxerui::MenuItem(
                                           "TCP", [pushDraft] { pushDraft(2); }));
                                       menu.Show(std::move(entries),
                                                 huxerui::MenuOptions{
                                                     .placement = {huxerui::AnchorSide::Below,
                                                                   huxerui::AnchorAlignment::Start}});
                                   })
                                   .With(menu.Anchor()),
                               huxerui::ScrollView{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(theme.spacing.small))}
                                   .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
                           }
                               .With(huxerui::Padding(theme.spacing.medium),
                                     huxerui::Spacing(theme.spacing.medium),
                                     huxerui::Background(theme.colors.surface_container_low),
                                     huxerui::CornerRadius(theme.shapes.large),
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    // 方向相关尺寸：竖排（Compact）限高撑宽；横排固定宽 260。
    return vertical ? std::move(island).With(huxerui::Frame{.max_height = 220.0F})
                    : std::move(island).With(huxerui::Frame{.width = 260.0F});
}
} // namespace

[[huxerui::composable]] huxerui::View RequestPage(huxerui::State<std::int64_t> opened) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    // 当前项目名（打开工作区时由主页写入领域 store）。
    // 未打开项目的兜底已删：主页整宽覆盖侧栏后本页在 opened==0 时不可达。
    std::string projectName = "（未知项目）";
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == opened.Get()) projectName = p.name;
    }

    // 内部标签页：每个打开的请求一个草稿；响应区状态为页面级（单引擎）。
    auto openDrafts = huxerui::UseState<std::vector<RequestDraft>>({});
    auto activeTab = huxerui::UseState<std::size_t>(0);
    auto listVersion = huxerui::UseState(0);
    auto inFlight = huxerui::UseState(false);
    auto responseBody = huxerui::UseState(std::string{"（尚未发送请求）"});
    auto responseHeaders = huxerui::UseState<std::vector<std::string>>({});

    // 响应式：Compact（<600pt）改上下堆叠，Medium/Expanded 保持左右双岛。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    const std::vector<RequestDraft> snapshot = openDrafts.Get();
    const std::size_t current =
        snapshot.empty() ? 0 : std::min(activeTab.Get(), snapshot.size() - 1);
    const int currentKind = snapshot.empty() ? 0 : snapshot[current].kind;
    // 右岛内容 Key：kind + 标签下标组合，切标签/切类型时正确重组。
    const std::int64_t contentKey = static_cast<std::int64_t>(current) * 4 + currentKind;

    // 右岛第二子节点按活跃草稿类型分派：HTTP 用编辑器（自套 ScrollView）；
    // WS/TCP 直接嵌入整页组件（内部自带 ScrollView/PageHeader/事件泵，勿再套
    // ScrollView，避免同轴嵌套滚动）。注意 WS/TCP 页内部用固定 kUid=1 引擎会话：
    // 多个同类型标签共享同一条连接。
    huxerui::View content =
        currentKind == 1
            ? WebSocketPage().With(huxerui::Grow(1.0F)).Key(contentKey)
        : currentKind == 2
            ? TcpPage().With(huxerui::Grow(1.0F)).Key(contentKey)
            : huxerui::View{huxerui::ScrollView{RequestEditor(openDrafts, current,
                                                              listVersion, inFlight,
                                                              responseBody, responseHeaders)
                                                    .Key(contentKey)}
                                .With(huxerui::ScrollBar(), huxerui::Grow(1.0F))};

    huxerui::View rightIsland =
        snapshot.empty()
            ? huxerui::View{huxerui::Column {
                                huxerui::Image(app::images::request)
                                    .With(huxerui::Frame{.width = 64.0F, .height = 64.0F},
                                          huxerui::Foreground(theme.colors.on_surface_variant)),
                                huxerui::Text("没有打开的请求", huxerui::TextRole::Title),
                                huxerui::Text("从左侧列表选择请求，或点击“＋ 新建”。",
                                              huxerui::TextRole::Body)
                                    .With(huxerui::Foreground(theme.colors.on_surface_variant)),
                            }
                                .With(huxerui::Spacing(theme.spacing.medium),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center))}
            : huxerui::View{huxerui::Column {
                                RequestTabStrip(openDrafts, activeTab),
                                std::move(content),
                            }
                                .With(huxerui::Spacing(theme.spacing.medium),
                                      huxerui::CrossAlign(
                                          huxerui::CrossAxisAlignment::Stretch))};

    // 右岛与左岛一级并列：同样的岛屿包裹（圆角 + surface_container_low 底）。
    rightIsland = std::move(rightIsland)
                      .With(huxerui::Background(theme.colors.surface_container_low),
                            huxerui::CornerRadius(theme.shapes.large),
                            huxerui::Padding(theme.spacing.medium));

    if (compact) {
        // Compact：上 = 请求列表（限高 220、宽度撑满），下 = 编辑区（撑满剩余）。
        return huxerui::Column {
                   RequestListIsland(openDrafts, activeTab, listVersion, projectName, true),
                   std::move(rightIsland).With(huxerui::Grow(1.0F)),
               }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return huxerui::Row {
               RequestListIsland(openDrafts, activeTab, listVersion, projectName, false),
               std::move(rightIsland).With(huxerui::Grow(1.0F)),
           }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
