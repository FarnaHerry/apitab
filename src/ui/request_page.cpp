// request_page.cpp — 请求工作区：左岛 = 当前项目的请求列表；右侧 HTTP 拆上下两岛
// （上 = 标签条 + 编辑器，下 = 响应区；WS/TCP 自包含整页仍单岛）。
// 发送走 store 持有的 curl 引擎（api::ApiEngine 抽象：send 纯入队，UI 协程
// PollWhile 轮询 takeResponse 取回结果，回 UI 线程写 State 并落历史）；
// 保存落到当前项目集合并刷新左岛列表。
#include <huxerui/huxerui.h>
#include <sweetedit_core/sweet_editor.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ui.h"
#include "draft.h"
#include "task_bridge.h"
#include "app_resources.h"
#include "syntax_grammars.h"

import apitab.api_engine;
import apitab.db;
import apitab.preferences;
import apitab.store.requests;
import apitab.utils;
import nlohmann.json;

namespace apitab::ui {

namespace {
// kMethodNames / kBodyTypeNames / KvRow / RequestDraft 等共享编辑类型已抽到
// draft.h（测试用例页 / Mock 页同用）；ToKeyValue / FromKeyValue 依赖
// api::KeyValue（模块类型），留在本 TU。

api::KeyValue ToKeyValue(const KvRow& row) {
    return api::KeyValue{.key = row.key.text,
                         .value = row.value.text,
                         .enabled = row.enabled,
                         .type = row.type.text,
                         .remark = row.remark.text};
}

KvRow FromKeyValue(const api::KeyValue& kv) {
    return KvRow{.key = huxerui::TextEditingValue{kv.key},
                 .value = huxerui::TextEditingValue{kv.value},
                 .type = huxerui::TextEditingValue{kv.type},
                 .remark = huxerui::TextEditingValue{kv.remark},
                 .enabled = kv.enabled};
}

// 值 → 类型自动推断（trimming 后判定）：空 → string；true/false → boolean；
// 能完整解析为整数/浮点（可带符号、小数点、科学计数）→ number；其余 → string。
std::string InferKvType(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty()) return "string";
    if (s == "true" || s == "false") return "boolean";
    std::string_view num = s;
    if (num.front() == '+') num.remove_prefix(1); // from_chars 不认前导 '+'
    double parsed = 0.0;
    const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), parsed,
                                           std::chars_format::general);
    if (ec == std::errc{} && ptr == num.data() + num.size()) {
        // from_chars 也能吃掉 inf/nan 字面量，限定首字符把它们排除在外。
        const char first = num.front();
        if ((first >= '0' && first <= '9') || first == '-' || first == '.') return "number";
    }
    return "string";
}

// KV 行的类型选择：行内扁平文本触发器（当前值 + ▾），点击弹自绘下拉选固定类型
// （做法同 MethodUrlBar 的方法触发器；菜单项回调在菜单层关闭后执行，脱离指针
// 事件路径，同步回写即可。选中项用深色填充底色，无对钩）。
[[huxerui::composable]] huxerui::View KvTypeSelect(
    std::string current, std::function<void(std::string)> onChanged) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto popup = huxerui::UsePopup();
    const std::string shown = current.empty() ? "string" : current;
    huxerui::View trigger = huxerui::Row {
        huxerui::Text(shown + " ▾", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
              huxerui::ClipChildren())
        .OnClick([popup, shown, onChanged = std::move(onChanged)] {
            std::vector<PopupMenuItem> items;
            items.reserve(kKvTypeNames.size());
            for (const std::string_view t : kKvTypeNames) {
                const std::string name{t};
                items.push_back(PopupMenuItem{.label = name,
                                              .on_click = [onChanged, name] { onChanged(name); },
                                              .checked = t == shown});
            }
            ShowPopupMenu(popup, std::move(items),
                          huxerui::PopupOptions{
                              .placement = {huxerui::AnchorSide::Below,
                                            huxerui::AnchorAlignment::Start}});
        });
    return std::move(trigger).With(popup.Anchor());
}

// KV 编辑表（回调风格）：rows 为快照，任何增删改经 onChanged 回写新 vector。
// 之所以不用 State 参数：行数据现在寄宿在请求草稿（RequestDraft）内部。
[[huxerui::composable]] huxerui::View KvTable(
    std::vector<KvRow> rows, const huxerui::ThemeSpec& theme, std::string keyLabel,
    std::string valueLabel, std::function<void(std::vector<KvRow>)> onChanged) {
    auto tasks = huxerui::UseTaskScope();
    // 表头与数据行共用同一套宽度约定：勾选框约 24pt，键/值/备注自适应拉伸，
    // 类型列固定 72pt。
    const auto typeWidth = huxerui::Frame{.width = 72.0F};
    std::vector<huxerui::View> children{
        huxerui::Row {
            huxerui::Text("", huxerui::TextRole::Label)
                .With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text(std::move(keyLabel), huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
            huxerui::Text(std::move(valueLabel), huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
            huxerui::Text("类型", huxerui::TextRole::Label).With(typeWidth),
            huxerui::Text("备注", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Foreground(theme.colors.on_surface_variant)),
    };

    // 自动追加语义：末尾恒渲染一个虚拟空行；对它写入即物化为真实行，
    // 重组后尾部再出现新的虚拟空行。✕ 只给真实行（虚拟行留占位保持行高）。
    for (std::size_t i = 0; i <= rows.size(); ++i) {
        const bool phantom = i == rows.size();
        const KvRow row = phantom ? KvRow{} : rows[i];
        // 行写入：i 越界（虚拟行）时物化新行，否则改写原行。
        // 虚拟行只在真正输入了键/值文本时才物化——聚焦/移动光标触发的
        // OnChanged（text 为空、仅选区变化）不追加新行；只填类型/备注也不物化。
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
                    .Label("键")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.key = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(row.value)
                    .Label("值")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.value = value;
                        // 值输入时自动重推断类型（不做"手动锁定"：手动下拉选过的
                        // 行，值再次输入仍按文本内容推断）。
                        updated.type = huxerui::TextEditingValue{InferKvType(value.text)};
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                // 类型列：固定值下拉（string/number/boolean），回写同样走 applyRow。
                KvTypeSelect(row.type.text, [row, i, applyRow](std::string type) {
                    KvRow updated = row;
                    updated.type = huxerui::TextEditingValue{std::move(type)};
                    applyRow(i, std::move(updated));
                })
                    .With(typeWidth),
                huxerui::TextField(row.remark)
                    .Label("备注")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.remark = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
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
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 内部标签拖拽载荷：按 uid 定位源/目标标签，与下标无关。
struct DraftTabDragPayload {
    std::uint64_t uid = 0;
};

// ---- 测试用例 / Mock：草稿编辑形态 ⇄ db 落库形态 ----
// 数字字段草稿侧用文本承载（受控 TextField），空/非法按"不校验"或默认值换算。

int ParseIntField(const huxerui::TextEditingValue& v, int fallback = 0) {
    const std::string s = trim(v.text);
    int out = fallback;
    if (!s.empty()) {
        std::from_chars(s.data(), s.data() + s.size(), out);
    }
    return out;
}

db::RequestTestCase CaseToDb(const TestCaseDraft& c) {
    db::RequestTestCase out;
    out.name = c.name.text;
    out.enabled = c.enabled;
    out.expectStatus = ParseIntField(c.expectStatus);
    const std::string ms = trim(c.maxMs.text);
    double parsed = 0.0;
    if (!ms.empty()) std::from_chars(ms.data(), ms.data() + ms.size(), parsed);
    out.maxMs = parsed;
    for (const KvRow& row : c.asserts) {
        if (row.key.text.empty()) continue;
        out.asserts.push_back({.path = row.key.text, .equals = row.value.text,
                               .enabled = row.enabled});
    }
    return out;
}

TestCaseDraft CaseFromDb(const db::RequestTestCase& c) {
    TestCaseDraft out;
    out.name = huxerui::TextEditingValue{c.name};
    out.enabled = c.enabled;
    if (c.expectStatus != 0) out.expectStatus = huxerui::TextEditingValue{std::to_string(c.expectStatus)};
    if (c.maxMs > 0.0) out.maxMs = huxerui::TextEditingValue{std::to_string(static_cast<long long>(c.maxMs))};
    for (const db::RequestAssertion& a : c.asserts) {
        out.asserts.push_back(KvRow{.key = huxerui::TextEditingValue{a.path},
                                    .value = huxerui::TextEditingValue{a.equals},
                                    .enabled = a.enabled});
    }
    return out;
}

db::RequestMock MockToDb(const MockDraft& m) {
    db::RequestMock out;
    out.enabled = m.enabled;
    out.status = ParseIntField(m.status, 200);
    out.delayMs = ParseIntField(m.delayMs);
    out.body = m.body.text;
    for (const KvRow& row : m.headers) {
        if (row.key.text.empty()) continue;
        out.headers.push_back(api::KeyValue{.key = row.key.text, .value = row.value.text,
                                            .enabled = row.enabled});
    }
    return out;
}

MockDraft MockFromDb(const db::RequestMock& m) {
    MockDraft out;
    out.enabled = m.enabled;
    out.status = huxerui::TextEditingValue{std::to_string(m.status)};
    out.delayMs = huxerui::TextEditingValue{std::to_string(m.delayMs)};
    out.body = huxerui::TextEditingValue{m.body};
    for (const api::KeyValue& kv : m.headers) out.headers.push_back(FromKeyValue(kv));
    return out;
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
    // 各文本类 body 独立存档：bodyContents 全量载入各类型槽位，切换类型
    // 互不影响；兼容字段 body 回落到当前类型槽位。fields 只取当前类型的。
    for (std::size_t i = 0; i < saved.bodyContents.size(); ++i) {
        if (!saved.bodyContents[i].text.empty())
            draft.bodies[i] = huxerui::TextEditingValue{saved.bodyContents[i].text};
    }
    if (draft.bodies[draft.bodyKindIndex].text.empty())
        draft.bodies[draft.bodyKindIndex] = huxerui::TextEditingValue{saved.body};
    if (draft.bodyKindIndex < saved.bodyContents.size()) {
        for (const api::KeyValue& kv : saved.bodyContents[draft.bodyKindIndex].fields)
            draft.bodyFields.push_back(FromKeyValue(kv));
    }
    for (const db::RequestTestCase& c : saved.testCases) draft.cases.push_back(CaseFromDb(c));
    draft.mock = MockFromDb(saved.mock);
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
            spec.body = draft.bodies[draft.bodyKindIndex].text;
            break;
    }
    return spec;
}

// 回写草稿中某个字段的便捷方式（MutateDraft）已抽到 draft.h，与测试用例页/
// Mock 页共用。

// 从响应头里提取 Set-Cookie 条目（键大小写不敏感），一行一个：
// "name = value; 属性..."（首个 '=' 换成 ' = ' 便于阅读，属性原样保留）。
std::vector<std::string> CookiesFromHeaders(const std::vector<api::KeyValue>& headers) {
    std::vector<std::string> lines;
    for (const api::KeyValue& h : headers) {
        std::string key = h.key;
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        if (key != "set-cookie") continue;
        std::string line = h.value;
        if (const std::size_t eq = line.find('='); eq != std::string::npos)
            line.insert(eq + 1, " "), line.insert(eq, " ");
        lines.push_back(std::move(line));
    }
    return lines;
}

// 响应区（右侧下岛）：独立重组作用域 —— responseTab/responseBody/responseHeaders/
// responseCookies 的变化只重组此区域，不扩散到整个编辑器（KV 表不受影响）。
// 标题与 Body/Headers/Cookies 切换固定在上，响应内容在内部 ScrollView 里滚动
// （垂直滚动条）。
[[huxerui::composable]] huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                                                   huxerui::State<std::vector<std::string>> responseHeaders,
                                                   huxerui::State<std::vector<std::string>> responseCookies,
                                                   const huxerui::ThemeSpec& theme) {
    auto responseTab = huxerui::UseState<std::size_t>(0);

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(font_size::kMonoBody),
                                  .foreground = theme.colors.on_surface};
    huxerui::View content = huxerui::Row{};
    if (responseTab.Get() == 0) {
        content = huxerui::View{huxerui::SelectionArea{
            huxerui::Text(responseBody.Get(), huxerui::TextRole::Body).Style(mono)}};
    } else if (responseTab.Get() == 1) {
        const std::vector<std::string> lines = responseHeaders.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        content = rows.empty()
            ? huxerui::View{huxerui::Text("（无响应头）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(2.0F))}};
    } else {
        const std::vector<std::string> lines = responseCookies.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        content = rows.empty()
            ? huxerui::View{huxerui::Text("（无 Cookie）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(2.0F))}};
    }

    return huxerui::Column {
        huxerui::Row {
            huxerui::Text("响应", huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
            huxerui::SegmentedButton({"Body", "Headers", "Cookies"}, responseTab)
                .OnChanged([responseTab](std::size_t index) { responseTab = index; }),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::ScrollView{std::move(content)}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 环境配置弹窗 ----

// 弹窗右侧：选中环境的配置表单（名称 / 基础 URL / 环境变量 KV 表 + 保存/关闭）。
// 以 envId 为 Key 挂进弹窗：切换选中环境时整个表单作用域重建，UseState 初始值
// 随之重取（初始值在 UseState 之前算好，组合体内不写 State）。
[[huxerui::composable]] huxerui::View EnvEditForm(huxerui::DialogContext ctx,
                                                  std::int64_t envId,
                                                  huxerui::State<int> envVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    const db::Environment* env = g_requests.findEnvironment(envId);
    if (env == nullptr) {
        // 防御：选中项刚被删除（正常路径在删除回调里已回落 selectedId）。
        return huxerui::Text("（环境已删除）", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant));
    }
    auto name = huxerui::UseState(huxerui::TextEditingValue{env->name});
    auto baseUrl = huxerui::UseState(huxerui::TextEditingValue{env->baseUrl});
    std::vector<KvRow> initialVars;
    for (const api::KeyValue& kv : env->variables) initialVars.push_back(FromKeyValue(kv));
    auto vars = huxerui::UseState<std::vector<KvRow>>(std::move(initialVars));

    return huxerui::Column {
        huxerui::ScrollView{huxerui::Column {
            huxerui::TextField(name.Get())
                .Label("名称")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([name](const huxerui::TextEditingValue& value) { name = value; }),
            huxerui::TextField(baseUrl.Get())
                .Label("基础 URL")
                .Placeholder("https://api.example.com")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([baseUrl](const huxerui::TextEditingValue& value) { baseUrl = value; }),
            huxerui::Text("环境变量（请求里用 {{变量名}} 引用）", huxerui::TextRole::Label),
            KvTable(vars.Get(), theme, "变量名", "变量值",
                    [vars](std::vector<KvRow> rows) { vars = std::move(rows); }),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
        huxerui::Row {
            // 保存不卸载本按钮（表单 Key 不变、State 保留）：同步写即可。
            huxerui::Button("保存").OnClick([envId, name, baseUrl, vars, envVersion, toast] {
                if (name.Get().text.empty()) {
                    toast.Show("环境名称不能为空");
                    return;
                }
                std::vector<api::KeyValue> kvs;
                kvs.reserve(vars.Get().size());
                for (const KvRow& row : vars.Get()) kvs.push_back(ToKeyValue(row));
                if (const std::string err = g_requests.updateEnvironment(
                        envId, name.Get().text, baseUrl.Get().text, kvs);
                    !err.empty()) {
                    toast.Show("保存失败: " + err);
                    return;
                }
                toast.Show("已保存");
                envVersion = envVersion.Get() + 1;
            }),
            huxerui::Button("关闭").OnClick([ctx] { ctx.Dismiss(); }),
        }
            // 两端对齐：保存在左、关闭在右，与其他弹窗一致。
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

// 环境配置弹窗（☰ 打开）：左侧环境列表（点击选中 / ＋ 新建 / ✎ 重命名 / ✕ 删除），
// 右侧选中环境的配置表单。envVersion 由 RequestPage 持有：环境增删改与保存后 bump，
// 弹窗与标签栏的环境下拉都按它重读 store。重命名输入框与删除确认框叠在本弹窗层之上
// （层内容捕获页面环境，UseDialog 照常可用）。
[[huxerui::composable]] huxerui::View EnvironmentDialog(huxerui::DialogContext ctx,
                                                        huxerui::State<int> envVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto selectedId = huxerui::UseState<std::int64_t>(g_requests.currentEnvId());
    // 悬停行 id（0 = 无）：Hover 事件非独占，悬停 ✎/✕ 时整行底色照样亮；
    // Leave 仅当仍是本行才清，防跨行误清。
    auto hoveredEnv = huxerui::UseState<std::int64_t>(0);
    auto renameValue = huxerui::UseState(huxerui::TextEditingValue{});
    (void)envVersion.Get(); // 订阅环境版本：增删改/保存后重组本弹窗

    const std::vector<db::Environment>& envs = g_requests.environments();
    // 选中项失效（被删）时的只读回落；正常路径由删除回调直接重置 selectedId。
    std::int64_t effective = selectedId.Get();
    if (effective != 0 && !g_requests.findEnvironment(effective))
        effective = g_requests.currentEnvId();

    std::vector<huxerui::View> rows;
    for (const db::Environment& e : envs) {
        const std::int64_t id = e.id;
        const bool selected = id == effective;
        rows.push_back(
            huxerui::Row {
                // 选中区：名称占满行宽；点击挂在整行 Row 上（见下方 .OnClick），
                // ✎/✕ 是最深命中节点、点击不冒泡，各触发各的。
                huxerui::Text(e.name.empty() ? "（未命名）" : e.name, huxerui::TextRole::Body)
                    .With(huxerui::Grow(1.0F), huxerui::ClipChildren()),
                // ✎ 重命名：弹输入框小弹窗（renameValue 寄宿本弹窗作用域）。
                huxerui::Text("✎", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = huxerui::Font::System(font_size::kCaption),
                                              .foreground = theme.colors.on_surface_variant})
                    .With(huxerui::Padding(4.0F))
                    .OnClick([dialog, tasks, toast, renameValue, envVersion, id,
                              name = e.name] {
                        renameValue = huxerui::TextEditingValue{name};
                        dialog.Show(
                            [tasks, toast, renameValue, envVersion,
                             id](huxerui::DialogContext renameCtx) -> huxerui::View {
                                return DialogCard(huxerui::Column {
                                    huxerui::Text("重命名环境", huxerui::TextRole::Title),
                                    huxerui::TextField(renameValue.Get())
                                        .Label("环境名称")
                                        .Variant(huxerui::TextFieldVariant::Outlined)
                                        .OnChanged([renameValue](
                                                       const huxerui::TextEditingValue& value) {
                                            renameValue = value;
                                        }),
                                    huxerui::Row {
                                        huxerui::Button("取消").OnClick(
                                            [renameCtx] { renameCtx.Dismiss(); }),
                                        huxerui::Button("确定")
                                            .OnClick([renameCtx, tasks, toast, renameValue,
                                                      envVersion, id] {
                                                if (renameValue.Get().text.empty()) {
                                                    toast.Show("环境名称不能为空");
                                                    return;
                                                }
                                                renameCtx.Dismiss();
                                                // 重组弹窗内容：推迟出指针事件路径
                                                tasks.Launch([=]() -> huxerui::Task<void> {
                                                    co_await huxerui::Delay(
                                                        std::chrono::duration<double>{0});
                                                    if (const std::string err =
                                                            g_requests.renameEnvironment(
                                                                id, renameValue.Get().text);
                                                        !err.empty()) {
                                                        toast.Show("重命名失败: " + err);
                                                        co_return;
                                                    }
                                                    envVersion = envVersion.Get() + 1;
                                                });
                                            }),
                                    }
                                        .With(huxerui::Spacing(8.0F),
                                              huxerui::MainAlign(
                                                  huxerui::MainAxisAlignment::SpaceBetween)),
                                }
                                                      .With(huxerui::Spacing(12.0F),
                                                            huxerui::Frame{.width = 320.0F},
                                                            huxerui::CrossAlign(
                                                                huxerui::CrossAxisAlignment::Stretch)));
                            },
                            huxerui::DialogOptions{});
                    }),
                // ✕ 删除：危险确认框（共享 helper，确认按钮染红）；删除重组本弹窗 → 推迟。
                huxerui::Text("✕", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = huxerui::Font::System(font_size::kCaption),
                                              .foreground = theme.colors.on_surface_variant})
                    .With(huxerui::Padding(4.0F))
                    .OnClick([dialog, tasks, selectedId, envVersion, id, name = e.name] {
                        ShowDangerConfirm(dialog, "删除环境",
                                          "确定删除环境「" + name + "」吗？此操作不可恢复。",
                                          "删除",
                                          [tasks, selectedId, envVersion, id] {
                                              tasks.Launch([=]() -> huxerui::Task<void> {
                                                  co_await huxerui::Delay(
                                                      std::chrono::duration<double>{0});
                                                  (void)g_requests.deleteEnvironment(id);
                                                  if (selectedId.Get() == id)
                                                      selectedId = g_requests.currentEnvId();
                                                  envVersion = envVersion.Get() + 1;
                                              });
                                          });
                    }),
            }
                .With(huxerui::Spacing(0.0F),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                      // 选中态底色与活跃标签 chip 一致（surface_container_highest）；
                      // 悬停（含悬停 ✎/✕，Hover 事件通道非独占）亮一档。
                      huxerui::Background(selected ? theme.colors.surface_container_highest
                                          : hoveredEnv.Get() == id
                                              ? theme.colors.surface_container_high
                                              : theme.colors.surface_container),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      // 行自身压掉默认 Indication：悬停反馈由手工底色承担，避免叠加。
                      huxerui::Indication{})
                // 点击整行 = 选中（只换右侧表单 Key，不卸载本行）：同步写即可。
                .OnClick([selectedId, id] { selectedId = id; })
                .On<huxerui::ViewEvents::Hover>([hoveredEnv, id](const huxerui::HoverEvent& e) {
                    if (e.type == huxerui::HoverEventType::Enter)
                        hoveredEnv = id;
                    else if (e.type == huxerui::HoverEventType::Leave && hoveredEnv.Get() == id)
                        hoveredEnv = 0;
                })
                .Key(id));
    }
    if (rows.empty()) {
        rows.push_back(huxerui::Text("暂无环境，点击上方 + 新建。", huxerui::TextRole::Body)
                           .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }

    // 右侧表单：Key = 选中环境 id，切换选中即重建表单作用域（初始值重取）。
    // effective==0 = 环境下拉选了"无"：无表单可编辑，只给提示。
    huxerui::View form = huxerui::Column{};
    if (envs.empty()) {
        form = huxerui::Text("暂无环境", huxerui::TextRole::Body)
                   .With(huxerui::Foreground(theme.colors.on_surface_variant));
    } else if (effective == 0) {
        form = huxerui::Text("未选择环境（当前为“无”）。", huxerui::TextRole::Body)
                   .With(huxerui::Foreground(theme.colors.on_surface_variant));
    } else {
        form = EnvEditForm(ctx, effective, envVersion).Key(effective);
    }

    return DialogCard(huxerui::Column {
        huxerui::Text("环境配置", huxerui::TextRole::Title),
        huxerui::Row {
            huxerui::Column {
                huxerui::Row {
                    huxerui::Text("环境", huxerui::TextRole::Label)
                        .With(huxerui::Grow(1.0F)),
                    // ＋ 新建：store 建默认名的环境并选中，随后可在右侧表单改名。
                    CircleButton("+", [tasks, toast, selectedId, envVersion] {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            if (const std::string err =
                                    g_requests.createEnvironment("新环境", "");
                                !err.empty()) {
                                toast.Show("新建环境失败: " + err);
                                co_return;
                            }
                            selectedId = g_requests.currentEnvId();
                            envVersion = envVersion.Get() + 1;
                        });
                    }, /*accent=*/false),
                }
                    .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                huxerui::ScrollView{huxerui::Column(std::move(rows))
                                        .With(huxerui::Spacing(theme.spacing.small))}
                    .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::Frame{.width = 200.0F},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            std::move(form),
        }
            .With(huxerui::Spacing(theme.spacing.medium), huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
                          .With(huxerui::Spacing(12.0F),
                                huxerui::Frame{.width = 680.0F, .height = 460.0F},
                                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 右岛顶部内部标签条：每个打开的草稿一个标签（点击切换 / ✕ 关闭），末尾 "＋" 新建；
// 最右侧为环境选择 + ☰ 合并控件（"无" + 当前项目环境，选中 = currentEnvId；☰ 开
// 环境配置弹窗）。envVersion 由 RequestPage 持有：环境 CRUD 后 bump，本条按它重读 store。
[[huxerui::composable]] huxerui::View RequestTabStrip(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<int> envVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    (void)envVersion.Get(); // 订阅环境版本：环境增删改/切换后重组本条
    const auto chipFont = huxerui::Font::System(font_size::kChip);
    const auto badgeFont =
        huxerui::Font::Monospace(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold);

    const std::vector<RequestDraft> snapshot = drafts.Get();
    // 悬停标签 uid：只有悬停的 chip 显示 ✕（0 = 无）。Hover 事件是包含
    // 生命周期：指针进入 chip 呈现边界发 Enter、离开才发 Leave，在子组件
    // （徽标/名称/✕）之间移动不重触发——挂在 chip 最外层即覆盖整 chip。
    auto hoveredChip = huxerui::UseState<std::uint64_t>(0);
    // 拖拽中的水平位移（Chrome 式贴条滑动，同顶级标签）：被拖 chip 的 uid +
    // X 位移（已按条内容范围钳制）。被拖 chip 本体变透明占位，视觉由条内
    // 覆盖层克隆接管（见下方 overlayChip）——覆盖层无任何事件 handler，
    // 命中测试穿透到下方静止 chip，drop 才能落到邻居上（否则被拖 chip 的
    // 偏移体永远顶在指针下，吞掉 drop）。
    auto dragUid = huxerui::UseState<std::uint64_t>(0);
    auto dragDx = huxerui::UseState(0.0F);
    // 拖动起点的槽位下标（Started 时记录）：覆盖层 X = 起点槽位 + 累计位移，
    // 与实时换位后的数据下标解耦，视觉连续不跳变。
    auto dragOrig = huxerui::UseState(0);
    // 让位滑动（StartSlide/SlideCell 见 ui.h）：tick 仅作重组触发器。
    auto slideCell = huxerui::UseState(std::make_shared<SlideCell>());
    auto slideTick = huxerui::UseState<std::uint64_t>(0);
    (void)slideTick.Get(); // 订阅：tween 每步 bump 触发重组
    // 固定 chip 宽度：拖拽换位/边缘钳制需要已知步进，同 Chrome 固定宽标签。
    // 步进 = chip 宽 + 分隔竖线(1pt) + 两侧间距（竖线作为 Row 子节点占布局，
    // 用 Opacity 显隐避免悬停时回流抖动）。
    constexpr float kChipDragWidth = 160.0F;
    const float chipStride = kChipDragWidth + 1.0F + 2.0F * theme.spacing.small;
    // 标签间分隔竖线：始终占布局（Opacity 显隐），相邻标签激活/悬停/被拖时
    // 隐藏；高度小于行高，上下留空隙不连通。
    auto chipDivider = [&](bool visible) {
        return huxerui::View{
            huxerui::Column{}.With(huxerui::Frame{.width = 1.0F, .height = 14.0F},
                                   huxerui::Background(theme.colors.outline),
                                   huxerui::Opacity(visible ? 1.0F : 0.0F))};
    };
    // 实时换位（Chrome 式：拖过邻居槽位即交换，不等松手）：把 uid 移动到
    // 目标槽位，activeTab 按 uid 跟随；被挤动的邻居加让位滑动。
    // keyed 重排不卸载节点，同步写即可。
    auto moveDraftTo = [drafts, activeTab, tasks, slideCell, slideTick,
                        chipStride](std::uint64_t uid, std::size_t desired) {
        std::vector<RequestDraft> copy = drafts.Get();
        auto findUid = [&copy](std::uint64_t u) {
            for (std::size_t k = 0; k < copy.size(); ++k)
                if (copy[k].uid == u) return k;
            return copy.size();
        };
        const std::size_t from = findUid(uid);
        if (from >= copy.size() || desired >= copy.size() || from == desired) return;
        // 让位滑动：from<desired 时 (from,desired] 的邻居左移一格（残量
        // +stride），反之 [desired,from) 右移一格（残量 -stride）。
        if (from < desired) {
            for (std::size_t k = from + 1; k <= desired; ++k)
                StartSlide(tasks, slideCell, slideTick,
                           static_cast<std::int64_t>(copy[k].uid), chipStride);
        } else {
            for (std::size_t k = desired; k < from; ++k)
                StartSlide(tasks, slideCell, slideTick,
                           static_cast<std::int64_t>(copy[k].uid), -chipStride);
        }
        const std::uint64_t activeUid =
            activeTab.Get() < copy.size() ? copy[activeTab.Get()].uid : 0;
        RequestDraft moved = std::move(copy[from]);
        copy.erase(copy.begin() + static_cast<long>(from));
        copy.insert(copy.begin() + static_cast<long>(desired), std::move(moved));
        for (std::size_t k = 0; k < copy.size(); ++k)
            if (copy[k].uid == activeUid) activeTab = k;
        drafts = copy;
    };
    std::vector<huxerui::View> chips;
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const bool active = i == activeTab.Get();
        const bool chipHovered = hoveredChip.Get() == snapshot[i].uid;
        // 背景默认不显示（透明）：激活 = 最高层级容器底，悬停 = 略深容器底，
        // 常态下只靠竖线分隔标签。
        const huxerui::Color fill =
            active ? theme.colors.surface_container_highest
                   : (chipHovered ? theme.colors.surface_container
                                  : huxerui::Color::Transparent());
        const huxerui::Color foreground =
            active ? theme.colors.on_surface : theme.colors.on_surface_variant;
        const std::string chipBadge = DraftKindBadge(snapshot[i]);
        chips.push_back(
            huxerui::Row {
                // 类型徽标：HTTP 显示方法名，WS/TCP 显示类型缩写。显式空
                // Indication：整 chip 的悬停反馈由外层 fill 承担，压掉内层默认高亮。
                // 徽标按 MethodColor 统一色表逐方法着色。
                huxerui::Text(chipBadge, huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{
                        .font = badgeFont,
                        .foreground = MethodColor(theme, chipBadge)})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(2.0F, 2.0F)),
                          huxerui::Indication{})
                    .OnClick([drafts, activeTab, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) activeTab = i;
                    }),
                huxerui::Text(DraftDisplayName(snapshot[i]), huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont, .foreground = foreground})
                    .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                          // 限宽给行尾 ✕ 留位（固定宽 160：徽标+名称+✕）。
                          huxerui::Frame{.max_width = 100.0F},
                          huxerui::Indication{})
                    .OnClick([drafts, activeTab, i] {
                        // 切换标签不卸载被点节点：同步写即可
                        if (i < drafts.Get().size()) activeTab = i;
                    }),
                // 弹性占位把 ✕ 顶到固定宽 chip 的右缘（Spacer 自带 Grow(1)）。
                huxerui::Spacer{},
                // 关闭钮：常驻、透明占位，悬停才显示（Opacity 只改绘制不动结构，
                // 避免悬停重组换子节点类型引起抖动）。透明时点击空转。
                huxerui::Text("✕", huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{.font = chipFont, .foreground = foreground})
                    .With(huxerui::Padding(4.0F),
                          huxerui::Opacity(hoveredChip.Get() == snapshot[i].uid ? 1.0F : 0.0F))
                    .OnClick([tasks, drafts, activeTab, i,
                              visible = hoveredChip.Get() == snapshot[i].uid] {
                        if (!visible) return; // 透明占位不响应点击
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
                    }),
            }
                .With(huxerui::Spacing(0.0F), huxerui::Background(fill),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                      huxerui::Frame{.width = kChipDragWidth, .height = 26.0F},
                      huxerui::ClipChildren(),
                      // 拖动时本体变透明占位：保留布局槽位与拖拽会话，
                      // 视觉由覆盖层克隆接管。
                      huxerui::Opacity(dragUid.Get() == snapshot[i].uid ? 0.0F : 1.0F),
                      // 让位滑动残量（非拖动标签恒 0）：实时换位时邻居从旧
                      // 槽位滑入新槽位。
                      huxerui::Offset(huxerui::Point{
                          SlideOffsetOf(slideCell.Get(),
                                        static_cast<std::int64_t>(snapshot[i].uid)),
                          0.0F}),
                      // 标签拖拽换位：限水平轴（axis=Horizontal，竖向移动不进入
                      // 拖拽），与内层徽标/文字的点击切换按阈值分胜负；无悬浮
                      // 拖影。请求级标签不支持拖出成独立窗口。
                      huxerui::DragSource(
                          DraftTabDragPayload{snapshot[i].uid},
                          huxerui::DragGesture{.axis = huxerui::Axis::Horizontal}))
                // 悬停显隐 ✕：Enter 记 uid，Leave 时仅当仍是本 chip 才清空
                // （防跨 chip 误清）。只写 hoveredChip，不做重活。
                .On<huxerui::ViewEvents::Hover>(
                    [hoveredChip, uid = snapshot[i].uid](const huxerui::HoverEvent& e) {
                        if (e.type == huxerui::HoverEventType::Enter)
                            hoveredChip = uid;
                        else if (e.type == huxerui::HoverEventType::Leave &&
                                 hoveredChip.Get() == uid)
                            hoveredChip = 0;
                    })
                // 拖拽开始：记录被拖 chip 与起点槽位。
                .On<huxerui::DragSourceEvents::Started>(
                    [dragUid, dragOrig, i, uid = snapshot[i].uid](
                        const huxerui::DragEvent&) {
                        dragUid = uid;
                        dragOrig = static_cast<int>(i);
                    })
                // 拖动中每帧：钳制后的累计 X 位移写入 dragDx（驱动覆盖层），
                // 并按"经过即换位"实时移动数据顺序——目标槽位 = 起点 +
                // round(位移/步进)。钳制范围 = 起点槽位到条内容两端，拖到
                // 容器外贴边停住。keyed 重排不卸载节点，同步写即可。
                .On<huxerui::DragSourceEvents::Changed>(
                    [dragUid, dragDx, dragOrig, chipStride, n = snapshot.size(),
                     moveDraftTo, uid = snapshot[i].uid](const huxerui::DragEvent& e) {
                        dragUid = uid;
                        const float orig = static_cast<float>(dragOrig.Get());
                        const float lo = -orig * chipStride;
                        const float hi =
                            static_cast<float>(n > 0 ? n - 1 : 0) * chipStride - orig * chipStride;
                        const float t = std::clamp(e.translation.x, lo, hi);
                        dragDx = t;
                        long desired =
                            static_cast<long>(orig) + std::lround(t / chipStride);
                        desired = std::clamp<long>(desired, 0,
                                                   static_cast<long>(n > 0 ? n - 1 : 0));
                        moveDraftTo(uid, static_cast<std::size_t>(desired));
                    })
                // 结束/取消：归零会移除覆盖层节点（卸载），推迟出指针事件路径。
                .On<huxerui::DragSourceEvents::Ended>(
                    [tasks, dragUid, dragDx](const huxerui::DragDropResult&) {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            dragUid = 0;
                            dragDx = 0.0F;
                        });
                    })
                .On<huxerui::DragSourceEvents::Canceled>(
                    [tasks, dragUid, dragDx](const huxerui::DragEvent&) {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            dragUid = 0;
                            dragDx = 0.0F;
                        });
                    })
                // Key 用稳定 uid：未保存草稿 savedId 恒为 0，不能再用下标兜底。
                .Key(static_cast<std::int64_t>(snapshot[i].uid)));
        // 标签间分隔竖线（最后一个草稿与 "＋" 之间不加）：相邻标签激活/
        // 悬停/被拖时隐藏，但始终占布局（Opacity 显隐，不回流）。
        if (i + 1 < snapshot.size()) {
            const bool sepVisible = !active && i + 1 != activeTab.Get() &&
                                    hoveredChip.Get() != snapshot[i].uid &&
                                    hoveredChip.Get() != snapshot[i + 1].uid &&
                                    dragUid.Get() != snapshot[i].uid &&
                                    dragUid.Get() != snapshot[i + 1].uid;
            chips.push_back(chipDivider(sepVisible));
        }
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

    // 拖拽覆盖层：被拖 chip 的视觉克隆（纯展示，无事件/悬停 handler——命中
    // 测试穿透到下方静止 chip）。Stack 中最后声明 = 绘制最上层（充当
    // z-index）。X = 拖拽起点槽位 + 钳制后的累计位移（与实时换位后的数据
    // 下标解耦，换位不引起视觉跳变）；Y 恒 0。
    huxerui::View overlayChip = huxerui::Row{};
    if (dragUid.Get() != 0) {
        for (std::size_t j = 0; j < snapshot.size(); ++j) {
            if (snapshot[j].uid != dragUid.Get()) continue;
            const bool overlayActive = j == activeTab.Get();
            const huxerui::Color overlayFill =
                overlayActive ? theme.colors.surface_container_highest
                              : theme.colors.surface_container;
            const huxerui::Color overlayForeground =
                overlayActive ? theme.colors.on_surface : theme.colors.on_surface_variant;
            const std::string overlayBadge = DraftKindBadge(snapshot[j]);
            overlayChip =
                huxerui::Row {
                    huxerui::Text(overlayBadge, huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{
                            .font = badgeFont,
                            .foreground = MethodColor(theme, overlayBadge)})
                        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(2.0F, 2.0F))),
                    huxerui::Text(DraftDisplayName(snapshot[j]), huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{.font = chipFont,
                                                  .foreground = overlayForeground})
                        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                              huxerui::Frame{.max_width = 100.0F}),
                    // 与本体一致：✕ 顶到右缘。
                    huxerui::Spacer{},
                    huxerui::Text("✕", huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{.font = chipFont,
                                                  .foreground = overlayForeground})
                        .With(huxerui::Padding(4.0F)),
                }
                    .With(huxerui::Spacing(0.0F), huxerui::Background(overlayFill),
                          huxerui::CornerRadius(theme.shapes.small),
                          huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                          huxerui::Frame{.width = kChipDragWidth, .height = 26.0F},
                          huxerui::ClipChildren(),
                          huxerui::Offset(huxerui::Point{
                              static_cast<float>(dragOrig.Get()) * chipStride + dragDx.Get(),
                              0.0F}));
            break;
        }
    }

    // 环境下拉：选项 = "无" + 当前项目环境（组合期按 envVersion 重读 store）。
    const std::vector<db::Environment>& envs = g_requests.environments();
    std::vector<std::string> envNames;
    envNames.reserve(envs.size() + 1);
    envNames.push_back("无");
    std::size_t currentEnv = 0;
    for (const db::Environment& e : envs) {
        if (e.id == g_requests.currentEnvId()) currentEnv = envNames.size();
        envNames.push_back(e.name.empty() ? "（未命名）" : e.name);
    }

    // 环境选择 + ☰ 合并控件（同 MethodUrlBar 思路）：一个共用描边圆角外框 +
    // Spacing(0)，左侧环境选择扁平触发器（文本"环境名 ▾"弹自绘下拉，选中项
    // 深色填充底色、无对钩；保持自绘而非官方 Select——Select 触发器自带描边
    // 外观，塞不进共用外框），中间 1pt 竖分隔线，右侧扁平 ☰（环境配置弹窗
    // 入口，去 CircleButton 的圆底）。外层 Row 交叉轴 Stretch 让分隔线拉满
    // 全高，两个触发器各自 CrossAlign(Center) 垂直居中。
    auto envMenu = huxerui::UsePopup();
    huxerui::View envTrigger =
        huxerui::Row {
            huxerui::Text(envNames.at(currentEnv) + " ▾", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface)),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .OnClick([envMenu, envNames = std::move(envNames), currentEnv, envVersion, toast,
                      envs] {
                std::vector<PopupMenuItem> items;
                items.reserve(envNames.size());
                for (std::size_t i = 0; i < envNames.size(); ++i) {
                    // 切换环境不卸载任何节点（菜单项回调在菜单层关闭后执行，
                    // 已脱离指针事件路径）：同步写即可。
                    items.push_back(PopupMenuItem{
                        .label = envNames[i],
                        .on_click = [envVersion, toast, envs, i] {
                            const std::int64_t id = i == 0 ? 0 : envs.at(i - 1).id;
                            if (const std::string err = g_requests.selectEnv(id);
                                !err.empty()) {
                                toast.Show("切换环境失败: " + err);
                                return;
                            }
                            envVersion = envVersion.Get() + 1;
                        },
                        .checked = i == currentEnv});
                }
                ShowPopupMenu(envMenu, std::move(items),
                              huxerui::PopupOptions{
                                  .placement = {huxerui::AnchorSide::Below,
                                                huxerui::AnchorAlignment::Start}});
            });
    envTrigger = std::move(envTrigger).With(envMenu.Anchor());

    huxerui::View envSettingsTrigger =
        huxerui::Row {
            huxerui::Text("☰", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface)),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            // ☰：环境配置弹窗（自定义内容层，DialogFactory）。
            .OnClick([dialog, envVersion] {
                dialog.Show(
                    [envVersion](huxerui::DialogContext ctx) -> huxerui::View {
                        return EnvironmentDialog(ctx, envVersion);
                    },
                    huxerui::DialogOptions{});
            });

    return huxerui::Row {
        // 标签 chips 占满剩余宽度（Grow 把环境区推到最右），溢出裁剪。
        // Stack 包裹：拖动时覆盖层克隆叠在 chips 之上（绘制最上层）。
        huxerui::Stack {
            huxerui::Row(std::move(chips))
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            std::move(overlayChip),
        }
            .With(huxerui::ClipChildren(), huxerui::Grow(1.0F)),
        huxerui::Row {
            std::move(envTrigger),
            // 竖分隔线：父 Row 交叉轴 Stretch 拉满全高。
            huxerui::Column{}.With(huxerui::Frame{.width = 1.0F},
                                   huxerui::Background(theme.colors.outline)),
            std::move(envSettingsTrigger),
        }
            .With(huxerui::Spacing(0.0F),
                  huxerui::Border(theme.colors.outline, 1.0F),
                  huxerui::CornerRadius(theme.shapes.small), huxerui::ClipChildren(),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::ClipChildren());
}

// 各 body 类型的 SweetLine 语法：JSON/XML/GraphQL 高亮，其余纯文本。
std::string_view SyntaxForBodyKind(std::size_t kind) {
    switch (kind) {
        case 1: return kJsonSyntax;    // Json
        case 5: return kXmlSyntax;     // Xml
        case 6: return kGraphqlSyntax; // GraphQL
        default: return kPlainSyntax;  // Text 等
    }
}

// Body 文本编辑器：SweetEditor 代码编辑器（行号/语法高亮/等宽度量），固定高度
// 不随内容长高，超长行横向滚动（wrap_mode=0），由所在分区的 ScrollView 统一
// 垂直滚动。SweetEditor 非受控：只有 document_key 变化才重载 initial_text，
// 格式化等外部改文本由父级持同一 controller 走 LoadDocument 刷新（见
// RequestEditor 的格式化按钮）。配色经本地补丁的 palette 跟随主题（EditorPalette）。
[[huxerui::composable]] huxerui::View BodyTextEditor(
    huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index,
    const RequestDraft& snapshot, const huxerui::ThemeSpec& theme,
    sweetedit_huxer::SweetEditorController controller) {
    const std::size_t kind = snapshot.bodyKindIndex;
    sweetedit_huxer::SweetEditorOptions options;
    options.palette = EditorPalette(theme);
    // document_key 绑定草稿 uid + body 类型：切请求或切 body 类型都会重载该
    // 类型自己存档的文本（各类型编辑框独立，输入不互通）。
    options.document_key = "body-" + std::to_string(snapshot.uid) + "-" +
                           std::to_string(kind);
    options.initial_text = snapshot.bodies[kind].text;
    options.syntax_json = std::string{SyntaxForBodyKind(kind)};
    options.wrap_mode = 0; // 不换行，横向滚动
    options.sticky_gutter = true; // 横向滚动时行号栏固定
    return sweetedit_huxer::SweetEditor(options, controller)
        .On<sweetedit_huxer::SweetEditorTextChanged>([drafts, index, controller, kind] {
            MutateDraft(drafts, index, [&](RequestDraft& d) {
                d.bodies[kind] = huxerui::TextEditingValue::FromText(controller.Text());
            });
        })
        .With(huxerui::Frame{.height = 220.0F}, huxerui::Grow(1.0F));
}

// 剥 JSON 注释（// 与 /* */；字符串内的不剥）。编辑区允许带注释的 JSON
// （allowJsonComments 语义），格式化前先剥掉再交给 nlohmann 解析。
std::string StripJsonComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool inString = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) out += in[++i];
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; out += c; continue; }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') ++i;
            if (i < in.size()) out += '\n';
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            ++i; // 跳过结尾 '/'
            continue;
        }
        out += c;
    }
    return out;
}

// XML 美化：标签换行 + 每层两空格缩进；声明/注释/自闭合标签不增减层级，
// 文本节点独行。标签配对不完整时抛异常（由调用方转 toast）。
std::string PrettyXml(const std::string& input) {
    std::string out;
    int depth = 0;
    bool first = true;
    auto emit = [&](std::string_view piece, int d) {
        if (!first) out += '\n';
        first = false;
        out.append(static_cast<std::size_t>(std::max(d, 0)) * 2, ' ');
        out += piece;
    };
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t lt = input.find('<', pos);
        const std::string text = trim(input.substr(pos, lt == std::string::npos
                                                         ? std::string::npos
                                                         : lt - pos));
        if (!text.empty()) emit(text, depth);
        if (lt == std::string::npos) break;
        const std::size_t gt = input.find('>', lt);
        if (gt == std::string::npos) throw std::runtime_error("XML 标签未闭合");
        const std::string tag = input.substr(lt, gt - lt + 1);
        if (tag.starts_with("</")) {
            emit(tag, --depth);
        } else if (tag.ends_with("/>") || tag.starts_with("<?") || tag.starts_with("<!")) {
            emit(tag, depth);
        } else {
            emit(tag, depth++);
        }
        pos = gt + 1;
    }
    if (depth != 0) throw std::runtime_error("XML 标签配对不完整");
    return out;
}

// 编辑器子页"测试用例"/"Mock"实现在 testcase_page.cpp / mock_page.cpp
// （声明在 ui.h）；居中空态样式各自文件内自带。

// 文档页：按当前请求草稿自动生成接口文档（只读；草稿任何编辑触发重组即刷新）。
[[huxerui::composable]] huxerui::View RequestDocPage(const RequestDraft& snapshot,
                                                     const std::string& envBaseUrl) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::string method = std::string{kMethodNames[snapshot.methodIndex]};
    // 组合 URL 与发送逻辑同规则：输入自带 URI scheme 原样，否则拼当前环境 baseUrl。
    const std::string fullUrl =
        g_requests.composeUrl(snapshot.url.text, 0, g_requests.currentEnvId());

    std::vector<huxerui::View> sections;
    sections.push_back(huxerui::Text(DraftDisplayName(snapshot), huxerui::TextRole::Title));
    sections.push_back(
        huxerui::Row {
            huxerui::Text(method, huxerui::TextRole::Label)
                .Style(huxerui::TextStyle{
                    .font = huxerui::Font::Monospace(font_size::kCaption)
                                .WithWeight(huxerui::FontWeight::SemiBold),
                    .foreground = MethodColor(theme, method)}),
            huxerui::Text(fullUrl.empty() ? "（未填写 URL）" : fullUrl,
                          huxerui::TextRole::Body),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    // KV 节：只列启用且有名目的条目；空节显示"无"。
    const auto kvSection = [&theme, &sections](const std::string& title,
                                               const std::vector<KvRow>& rows) {
        sections.push_back(huxerui::Text(title, huxerui::TextRole::Label)
                               .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        bool any = false;
        for (const KvRow& row : rows) {
            if (!row.enabled || row.key.text.empty()) continue;
            any = true;
            sections.push_back(huxerui::Row {
                huxerui::Text(row.key.text, huxerui::TextRole::Body)
                    .With(huxerui::Frame{.min_width = 120.0F}),
                huxerui::Text(row.value.text, huxerui::TextRole::Body)
                    .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            }
                .With(huxerui::Spacing(theme.spacing.small)));
        }
        if (!any)
            sections.push_back(huxerui::Text("无", huxerui::TextRole::Body)
                                   .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    };
    kvSection("请求参数", snapshot.params);
    kvSection("请求头", snapshot.headers);
    kvSection("Cookies", snapshot.cookies);

    // 请求体：Form 类按字段表，文本类展示原文（等宽），"无"则整节跳过。
    if (snapshot.bodyKindIndex == 3 || snapshot.bodyKindIndex == 4) {
        kvSection(std::format("请求体（{}）", kBodyTypeNames[snapshot.bodyKindIndex]),
                  snapshot.bodyFields);
    } else if (snapshot.bodyKindIndex != 0) {
        sections.push_back(
            huxerui::Text(
                std::format("请求体（{}）", kBodyTypeNames[snapshot.bodyKindIndex]),
                huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        const std::string& body = snapshot.bodies[snapshot.bodyKindIndex].text;
        sections.push_back(
            huxerui::Text(body.empty() ? "（空）" : body, huxerui::TextRole::Body)
                .Style(huxerui::TextStyle{
                    .font = huxerui::Font::Monospace(font_size::kMonoBody),
                    .foreground = theme.colors.on_surface}));
    }

    return huxerui::ScrollView{huxerui::Column(std::move(sections))
                                   .With(huxerui::Spacing(theme.spacing.small),
                                         huxerui::CrossAlign(
                                             huxerui::CrossAxisAlignment::Stretch))}
        .With(huxerui::ScrollBar(), huxerui::Grow(1.0F));
}

// 右上岛编辑区：选择行（调试/文档/测试用例/Mock + 短名称修改框）+ 调试页的
// 方法/URL/发送/保存/⋮ 操作栏 + Params/Headers/Cookies/Body 分区。固定部分
// （选择行/操作栏/分区切换/Body 类型行）不滚动；分区内容（KV 表或 Body 编辑器）
// 在内部 ScrollView 里滚动并带垂直滚动条——所有 Body 类型输入框（含 Form 类
// KV 表）都有滚动条。响应区已拆为独立下岛（见 RequestPage，仅调试页显示）。
[[huxerui::composable]] huxerui::View RequestEditor(
    huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index,
    huxerui::State<std::size_t> activeTab,
    huxerui::State<int> listVersion, huxerui::State<bool> inFlight,
    huxerui::State<std::string> responseBody,
    huxerui::State<std::vector<std::string>> responseHeaders,
    huxerui::State<std::vector<std::string>> responseCookies,
    huxerui::State<int> envVersion, huxerui::State<std::size_t> pageTab) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto overflow = huxerui::UsePopup(); // ⋮ 溢出菜单（删除当前请求；自绘 PopupMenu）
    auto dialog = huxerui::UseDialog(); // 删除确认弹窗（危险确认，确认按钮染红）
    auto section = huxerui::UseState<std::size_t>(0); // 0=Params 1=Headers 2=Cookies 3=Body
    auto sendSeq = huxerui::UseState<std::uint64_t>(0); // 发送代际：取消/取代使旧结果失效
    auto sendTask = huxerui::UseState<huxerui::TaskHandle>(huxerui::TaskHandle{});
    // Body 编辑器控制器：格式化按钮与 BodyTextEditor 共享（SweetEditor 非受控，
    // 外部改文本须 LoadDocument）。在顶层创建保证切分区时 UseState 槽位稳定。
    auto bodyEditorController = sweetedit_huxer::UseSweetEditorController();
    (void)envVersion.Get(); // 订阅环境版本：切环境后重组，刷新 baseUrl 显示段

    const std::vector<RequestDraft> all = drafts.Get();
    if (index >= all.size()) {
        return huxerui::Text("（标签页已关闭）", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant));
    }
    const RequestDraft snapshot = all[index];

    std::vector<huxerui::View> children;
    // 当前环境的基础 URL（操作栏 baseUrl 显示段与文档页组合 URL 用；无环境/为空时
    // 显示段不渲染）。
    std::string envBaseUrl;
    if (const db::Environment* env = g_requests.findEnvironment(g_requests.currentEnvId()))
        envBaseUrl = env->baseUrl;

    // 选择行：调试/文档/测试用例/Mock 切换 + 短名称修改框（原整宽名称行并入此行）。
    children.push_back(huxerui::Row {
        huxerui::SegmentedButton({"调试", "文档", "测试用例", "Mock"}, pageTab)
            .OnChanged([pageTab](std::size_t i) { pageTab = i; }),
        huxerui::TextField(snapshot.name)
            .Label("名称")
            .Placeholder("请求名称")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([drafts, index](const huxerui::TextEditingValue& value) {
                MutateDraft(drafts, index, [&](RequestDraft& d) { d.name = value; });
            })
            .With(huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    if (pageTab.Get() != 0) {
        // 文档页按当前草稿自动生成；测试用例/Mock 为独立编辑页（自有文件）。
        if (pageTab.Get() == 1)
            children.push_back(RequestDocPage(snapshot, envBaseUrl));
        else if (pageTab.Get() == 2)
            children.push_back(TestCasePage(snapshot, drafts, index));
        else
            children.push_back(MockPage(snapshot, drafts, index));
        return huxerui::Column(std::move(children))
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    // 分区切换条固定在滚动区外。
    huxerui::View sectionTabs = huxerui::View{huxerui::SegmentedButton(
        {"Params", "Headers", "Cookies", "Body"}, section)
                                      .OnChanged([section](std::size_t i) { section = i; })};
    // sectionFixed：分区各自的固定头（仅 Body 有：类型选择行 + 条件渲染的格式化行）；
    // sectionContent：进内部 ScrollView 的滚动内容（KV 表 / Body 编辑器）。
    std::optional<huxerui::View> sectionFixed;
    huxerui::View sectionContent = huxerui::Column{};
    switch (section.Get()) {
        case 0:
            sectionContent = KvTable(
                snapshot.params, theme, "参数名", "参数值",
                [drafts, index](std::vector<KvRow> rows) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.params = std::move(rows); });
                });
            break;
        case 1:
            sectionContent = KvTable(
                snapshot.headers, theme, "头名称", "头值",
                [drafts, index](std::vector<KvRow> rows) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.headers = std::move(rows); });
                });
            break;
        case 2:
            sectionContent = KvTable(
                snapshot.cookies, theme, "Cookie 名", "Cookie 值",
                [drafts, index](std::vector<KvRow> rows) {
                    MutateDraft(drafts, index,
                                [&](RequestDraft& d) { d.cookies = std::move(rows); });
                });
            break;
        default: {
            // Body 固定头：类型选择行（只有 SegmentedButton，下标 = api::BodyKind）；
            // 选中 JSON/XML 时下一行出现"格式化"按钮（左对齐；JSON 剥注释后
            // nlohmann dump(2)，XML 标签缩进换行），其余类型整行不渲染。
            // 固定在滚动区外，滚动时仍可见。
            std::vector<huxerui::View> bodyFixed{
                huxerui::Row {
                    huxerui::SegmentedButton(
                        {"无", "JSON", "Text", "Form URL-Encoded", "Form-Data", "XML", "GraphQL"},
                        snapshot.bodyKindIndex)
                        .OnChanged([drafts, index](std::size_t kind) {
                            MutateDraft(drafts, index,
                                        [kind](RequestDraft& d) { d.bodyKindIndex = kind; });
                        }),
                },
            };
            if (snapshot.bodyKindIndex == 1 || snapshot.bodyKindIndex == 5) {
                bodyFixed.push_back(huxerui::Row {
                    huxerui::Button("格式化").OnClick(
                        [drafts, index, toast, tasks, bodyEditorController] {
                        const std::vector<RequestDraft> current = drafts.Get();
                        if (index >= current.size()) return;
                        const std::size_t kind = current[index].bodyKindIndex;
                        if (kind != 1 && kind != 5) { // 仅 JSON / XML 可格式化
                            toast.Show(kind == 6 ? "GraphQL 暂不支持格式化"
                                                 : "当前类型无需格式化");
                            return;
                        }
                        const std::string text = current[index].bodies[kind].text;
                        if (text.empty()) return;
                        const std::uint64_t uid = current[index].uid;
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            // 大 body 的解析/美化是 CPU 重活：派到任务线程执行，
                            // 结果回 UI 线程后再写 State（见 task_bridge.h）。
                            try {
                                std::string pretty = co_await RunOnTaskThread([text, kind] {
                                    return kind == 1
                                               ? nlohmann::json::parse(StripJsonComments(text)).dump(2)
                                               : PrettyXml(text);
                                });
                                // SweetEditor 非受控：写 draft 之外还须 LoadDocument
                                // 让编辑器刷新（key 与 BodyTextEditor 一致，uid+类型）。
                                bodyEditorController.LoadDocument(
                                    "body-" + std::to_string(uid) + "-" + std::to_string(kind),
                                    pretty,
                                    std::string{SyntaxForBodyKind(kind)});
                                MutateDraft(drafts, index, [&](RequestDraft& d) {
                                    d.bodies[kind] = huxerui::TextEditingValue{std::move(pretty)};
                                });
                            } catch (const std::exception& e) {
                                toast.Show(std::string{"格式化失败: "} + e.what());
                            }
                        });
                    }),
                });
            }
            sectionFixed = huxerui::View{huxerui::Column(std::move(bodyFixed))
                                             .With(huxerui::Spacing(theme.spacing.small),
                                                   huxerui::CrossAlign(
                                                       huxerui::CrossAxisAlignment::Stretch))};
            switch (snapshot.bodyKindIndex) {
                case 0: // 无
                    sectionContent = huxerui::View{
                        huxerui::Text("无请求体", huxerui::TextRole::Body)
                            .With(huxerui::Foreground(theme.colors.on_surface_variant))};
                    break;
                case 3: // Form URL-Encoded
                case 4: // Form-Data
                    sectionContent = KvTable(
                        snapshot.bodyFields, theme, "字段名", "字段值",
                        [drafts, index](std::vector<KvRow> rows) {
                            MutateDraft(drafts, index,
                                        [&](RequestDraft& d) { d.bodyFields = std::move(rows); });
                        });
                    break;
                default: // JSON/Text/XML/GraphQL：带行号的代码编辑器
                    sectionContent =
                        BodyTextEditor(drafts, index, snapshot, theme, bodyEditorController);
                    break;
            }
            break;
        }
    }

    // 操作栏：方法+URL 合并控件（占满）+ 发送/取消 + 保存 + ⋮（删除）。
    children.push_back(huxerui::Row {
            MethodUrlBar(
                std::vector<std::string>(kMethodNames.begin(), kMethodNames.end()),
                snapshot.methodIndex,
                [drafts, index](std::size_t method) {
                    MutateDraft(drafts, index,
                                [method](RequestDraft& d) { d.methodIndex = method; });
                },
                snapshot.url,
                [drafts, index](const huxerui::TextEditingValue& value) {
                    MutateDraft(drafts, index, [&](RequestDraft& d) { d.url = value; });
                },
                std::move(envBaseUrl),
                "https://api.example.com/v1/resource"),
            // 发送/取消：在途时按钮变"取消"。传输由 store 持有的引擎（api::ApiEngine
            // 抽象，curl 实现）在后台工作线程执行：send 纯入队立即返回，UI 协程
            // PollWhile 轮询 takeResponse 取回结果（恢复点恒为 UI 线程，见
            // task_bridge.h）。取消 = sendSeq 代际作废在途结果 + 引擎协作打断 +
            // 轮询协程 Cancel。
            huxerui::Button(inFlight.Get() ? "取消" : "发送").OnClick([=] {
                // 发送/取消都会翻转被点按钮的文案（重组本子树），State 写入与
                // 引擎操作整体推迟出指针事件路径（CLAUDE.md 约定 6；同步写曾在
                // pointer-up 处理中引发事件循环卡死/段错误）。
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    if (inFlight.Get()) {
                        sendSeq += 1;
                        g_requests.cancelSend();
                        sendTask.Get().Cancel();
                        inFlight = false;
                        responseBody = "已取消";
                        co_return;
                    }
                    const std::vector<RequestDraft> current = drafts.Get();
                    if (index >= current.size()) co_return;
                    api::RequestSpec spec = SpecFromDraft(current[index]);
                    if (spec.url.empty()) {
                        toast.Show("URL 不能为空");
                        co_return;
                    }
                    // Mock 拦截：草稿"Mock"子页启用时不发真实请求、不写历史，
                    // 按模拟定义直接返回响应（定义见 mock_page.cpp / MockDraft）。
                    // 代际作废语义与正常路径一致：延迟期间被取消/取代则结果不投递。
                    if (current[index].mock.enabled) {
                        const MockDraft mock = current[index].mock;
                        sendSeq += 1;
                        const std::uint64_t seq = sendSeq.Get();
                        inFlight = true;
                        responseBody = "Mock 响应中…";
                        responseHeaders = {};
                        responseCookies = {};
                        const int status = ParseIntField(mock.status, 200);
                        const int delayMs = std::max(0, ParseIntField(mock.delayMs, 0));
                        std::vector<api::KeyValue> headers;
                        for (const KvRow& row : mock.headers) {
                            if (!row.enabled || row.key.text.empty()) continue;
                            headers.push_back(api::KeyValue{.key = row.key.text,
                                                            .value = row.value.text});
                        }
                        if (delayMs > 0) {
                            co_await huxerui::Delay(std::chrono::duration<double>{
                                delayMs / 1000.0});
                        }
                        if (seq != sendSeq.Get()) co_return; // 已取消/被新请求取代
                        api::ResponseView view;
                        view.ok = true;
                        view.status = status;
                        view.totalMs = static_cast<double>(delayMs);
                        view.sizeBytes = static_cast<std::int64_t>(mock.body.text.size());
                        view.body = mock.body.text;
                        view.headers = std::move(headers);
                        responseBody = std::format(
                            "MOCK · HTTP {} · {}ms · {} bytes\n\n{}", view.status,
                            view.totalMs, view.sizeBytes, view.body);
                        std::vector<std::string> lines;
                        for (const api::KeyValue& h : view.headers)
                            lines.push_back(h.key + ": " + h.value);
                        responseHeaders = lines;
                        responseCookies = CookiesFromHeaders(view.headers);
                        inFlight = false;
                        co_return;
                    }
                    const std::int64_t requestId = current[index].savedId;
                    const api::RequestSpec finalSpec = g_requests.finalizeSpec(spec);
                    sendSeq += 1;
                    const std::uint64_t seq = sendSeq.Get();
                    inFlight = true;
                    responseBody = "发送中…";
                    responseHeaders = {};
                    responseCookies = {};
                    g_requests.sendViaEngine(finalSpec);
                    sendTask = tasks.Launch([=]() -> huxerui::Task<void> {
                        // 引擎结果按 30ms 节拍轮询取回（totalMs 由引擎计时）；恢复点
                        // 恒为 UI 线程，直接写 State 并落历史（见 task_bridge.h）。
                        api::ResponseView view;
                        co_await PollWhile(std::chrono::duration<double>{0.03},
                                           [&view] { return !g_requests.takeResponse(view); });
                        if (seq != sendSeq.Get()) co_return; // 已取消/被新请求取代
                        g_requests.recordHistory(requestId, finalSpec.method, finalSpec.url, view);
                        if (view.ok) {
                            responseBody = std::format("HTTP {} · {} · {} bytes\n\n{}", view.status,
                                                       view.totalMs, view.sizeBytes, view.body);
                            std::vector<std::string> lines;
                            for (const api::KeyValue& h : view.headers)
                                lines.push_back(h.key + ": " + h.value);
                            responseHeaders = lines;
                            responseCookies = CookiesFromHeaders(view.headers);
                        } else {
                            responseBody = "请求失败: " + view.error;
                        }
                        inFlight = false;
                    });
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
                // 文本类 body 各自归档（JSON/Text/XML/GraphQL 独立编辑互不影响），
                // form 类 body 的结构化字段按当前类型落进 bodyContents。
                for (std::size_t i = 0; i < saved.bodyContents.size(); ++i)
                    saved.bodyContents[i].text = draft.bodies[i].text;
                const std::size_t bodyIndex = static_cast<std::size_t>(spec.bodyKind);
                if (bodyIndex < saved.bodyContents.size())
                    saved.bodyContents[bodyIndex].fields = spec.bodyFields;
                // 测试用例 / Mock 随请求一起落库（保存按钮是全量覆盖语义）。
                for (const TestCaseDraft& c : draft.cases) saved.testCases.push_back(CaseToDb(c));
                saved.mock = MockToDb(draft.mock);
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
            // ⋮ 溢出菜单：删除当前请求条（自绘 PopupMenu，删除项 hover 才显红；
            // 点击先弹危险确认框；已保存的连集合一起删，并关掉本标签）。
            // 删除会卸载本编辑器 → 确认回调里推迟出指针事件路径。
            CircleButton(
                "⋮",
                [overflow, dialog, tasks, drafts, activeTab, listVersion, index] {
                    std::vector<RequestDraft> snapshot = drafts.Get();
                    if (index >= snapshot.size()) return;
                    const bool saved = snapshot[index].savedId != 0;
                    const std::string name = DraftDisplayName(snapshot[index]);
                    ShowPopupMenu(
                        overflow,
                        {PopupMenuItem{
                            .label = "删除",
                            .on_click = [dialog, tasks, drafts, activeTab, listVersion, index,
                                         saved, name] {
                                ShowDangerConfirm(
                                    dialog, "删除请求",
                                    saved ? "确定删除请求「" + name +
                                                "」吗？将从集合中删除，此操作不可恢复。"
                                          : "确定删除草稿「" + name +
                                                "」吗？未保存的内容将丢失。",
                                    "删除", [tasks, drafts, activeTab, listVersion, index] {
                                        tasks.Launch([=]() -> huxerui::Task<void> {
                                            co_await huxerui::Delay(
                                                std::chrono::duration<double>{0});
                                            std::vector<RequestDraft> copy = drafts.Get();
                                            if (index >= copy.size()) co_return;
                                            if (copy[index].savedId != 0) {
                                                (void)g_requests.remove(copy[index].savedId);
                                                listVersion = listVersion.Get() + 1;
                                            }
                                            copy.erase(copy.begin() +
                                                       static_cast<long>(index));
                                            drafts = copy;
                                            if (!copy.empty() && activeTab.Get() >= copy.size())
                                                activeTab = copy.size() - 1;
                                        });
                                    });
                            },
                            .danger = PopupMenuDanger::kHoverRed}},
                        huxerui::PopupOptions{
                            .placement = {huxerui::AnchorSide::Below,
                                          huxerui::AnchorAlignment::End}});
                },
                /*accent=*/false)
                .With(overflow.Anchor()),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
        children.push_back(std::move(sectionTabs));
        if (sectionFixed.has_value()) children.push_back(std::move(*sectionFixed));
        // 分区内容：内部滚动 + 垂直滚动条（Grow 撑满上岛剩余高度）。
        children.push_back(huxerui::ScrollView{std::move(sectionContent)}
                               .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)));
        return huxerui::Column(std::move(children))
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 拖拽载荷：请求行 / 分组行（typed payload，DragSource/DropTarget 按类型匹配）。
struct RequestDragPayload {
    std::int64_t requestId = 0;
};
struct GroupDragPayload {
    std::int64_t groupId = 0;
};

// 行尾 ⋮ 菜单按钮：常驻、默认透明（Opacity 是 paint 修饰符，悬停显隐只改绘制、
// 不换子节点类型/数量——避免悬停重组时子树卸载重建引发 hover 振荡抖动）；
// 透明时点击空转。行悬停态由行最外层容器的 Hover 事件维护（按钮在行边界内，
// 悬停 ⋮ 不退出行的悬停态），本按钮不再单独跟踪。必须是独立 composable：
// 菜单锚点（LayerAnchor）只能挂载在一个 View 上（"presentation anchor must be
// mounted on only one View"），每个按钮实例需要在自己的作用域里 UsePopup 拿独立锚点。
// 菜单用自绘 PopupMenu（危险项 hover 才显红）。
[[huxerui::composable]] huxerui::View RowMenuButton(bool visible,
                                                    std::vector<PopupMenuItem> items) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto popup = huxerui::UsePopup();
    return huxerui::Text("⋮", huxerui::TextRole::Label)
        .Style(huxerui::TextStyle{.font = huxerui::Font::System(font_size::kCaption),
                                  .foreground = theme.colors.on_surface_variant})
        .With(huxerui::Padding(4.0F), popup.Anchor(),
              huxerui::Opacity(visible ? 1.0F : 0.0F))
        .OnClick([popup, items = std::move(items), visible] {
            if (!visible) return; // 透明占位不响应点击
            ShowPopupMenu(popup, std::move(items),
                          huxerui::PopupOptions{
                              .placement = {huxerui::AnchorSide::Below,
                                            huxerui::AnchorAlignment::Start}});
        });
}

// 左岛：当前项目的请求树（分组按 parentId 层级渲染为可折叠节点，请求为叶子，
// 点击开标签，行尾 ⋮ / 右键菜单做重命名与删除）+ 头部圆形 "+" 新建类型菜单。
// vertical=true 用于 Compact 视口：列表改为顶部横岛（限高、宽度撑满）。
[[huxerui::composable]] huxerui::View RequestListIsland(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<int> listVersion, bool vertical) {

    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto menu = huxerui::UseMenu();
    auto ctxMenu = huxerui::UsePopup(); // 行右键菜单（自绘 PopupMenu）：ShowAt 无需锚点，所有行共享
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    (void)listVersion.Get(); // 订阅列表版本：保存/删除后触发本岛重组

    // 新建分组弹窗的受控状态（"+" 菜单 → 新建分组 时重置）。
    auto newGroupName = huxerui::UseState(huxerui::TextEditingValue{});
    auto newGroupMode = huxerui::UseState<std::size_t>(0); // 0=仅名称 1=路径

    // 悬停行：请求行 = 请求 id，分组行 = -分组 id（两表 id 空间会撞，用符号区分），
    // 0 = 无。Hover 事件是包含生命周期：进入行呈现边界发 Enter、离开才发 Leave，
    // 行内子组件（打开区/⋮ 按钮）之间移动不重触发——挂在行最外层即覆盖整行。
    auto hoveredRow = huxerui::UseState<std::int64_t>(0);
    // 重命名弹窗的输入值（打开前预填当前名称）。
    auto renameValue = huxerui::UseState(huxerui::TextEditingValue{});

    const auto methodFont =
        huxerui::Font::Monospace(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold);

    // 折叠状态：收起的分组 id 集（默认全部展开）。切换只重组本岛，
    // 不卸载被点的分组行本身，同步写即可。
    auto collapsed = huxerui::UseState<std::vector<std::int64_t>>({});

    std::vector<huxerui::View> rows;
    const std::vector<db::SavedRequest>& saved = g_requests.list();
    const std::vector<db::Group>& groups = g_requests.groups();

    // 行底色只给"被选中"的条目——当前活跃标签对应的集合项；其余行透明。
    const std::vector<RequestDraft> openTabs = drafts.Get();
    const std::int64_t activeSavedId =
        activeTab.Get() < openTabs.size() ? openTabs[activeTab.Get()].savedId : -1;

    // 重命名弹窗（请求/分组共用）：预填当前名，apply 回调落库并返回错误串
    // （空 = 成功），成功后 bump listVersion 重组本岛。布局同「新建分组」弹窗。
    auto showRenameDialog =
        [dialog, tasks, toast, listVersion, renameValue](
            const std::string& currentName,
            const std::function<std::string(const std::string&)>& apply) {
            renameValue = huxerui::TextEditingValue{currentName};
            dialog.Show(
                [tasks, toast, listVersion, renameValue,
                 apply](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("重命名", huxerui::TextRole::Title),
                        huxerui::TextField(renameValue.Get())
                            .Label("名称")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([renameValue](const huxerui::TextEditingValue& value) {
                                renameValue = value;
                            }),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                            huxerui::Button("确定")
                                .OnClick([ctx, tasks, toast, listVersion, renameValue, apply] {
                                    const std::string name = renameValue.Get().text;
                                    if (name.empty()) {
                                        toast.Show("名称不能为空");
                                        return;
                                    }
                                    ctx.Dismiss();
                                    // 落库后重组左岛：推迟出指针事件路径
                                    tasks.Launch([=]() -> huxerui::Task<void> {
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        if (const std::string err = apply(name); !err.empty()) {
                                            toast.Show("重命名失败: " + err);
                                            co_return;
                                        }
                                        listVersion = listVersion.Get() + 1;
                                    });
                                }),
                        }
                            .With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{});
        };

    // 请求行的菜单条目（重命名/删除）：行尾 ⋮ 按钮与右键菜单共用，
    // 按行现场构造，避免两处逻辑分叉。自绘 PopupMenu：删除项 hover 才显红。
    auto requestEntries = [showRenameDialog, dialog, tasks, drafts, activeTab,
                           listVersion](const db::SavedRequest& r) {
        const std::int64_t id = r.id;
        return std::vector<PopupMenuItem>{
            PopupMenuItem{
                .label = "重命名",
                .on_click = [showRenameDialog, drafts, id, currentName = r.name] {
                    // 重命名后同步已打开标签的名字。
                    showRenameDialog(currentName, [drafts, id](const std::string& name) {
                        const std::string err = g_requests.renameRequest(id, name);
                        if (err.empty()) {
                            std::vector<RequestDraft> copy = drafts.Get();
                            for (RequestDraft& d : copy)
                                if (d.savedId == id)
                                    d.name = huxerui::TextEditingValue{name};
                            drafts = copy;
                        }
                        return err;
                    });
                }},
            PopupMenuItem{
                .label = "删除",
                .on_click = [dialog, tasks, drafts, activeTab, listVersion, id,
                             name = r.name] {
                    // 删除前先弹危险确认框（确认按钮染红）；真正删行仍在确认回调里
                    // 推迟出指针事件路径。
                    ShowDangerConfirm(dialog, "删除请求",
                                      "确定删除请求「" + (name.empty() ? "未命名" : name) +
                                          "」吗？此操作不可恢复。",
                                      "删除", [tasks, drafts, activeTab, listVersion, id] {
                                          tasks.Launch([=]() -> huxerui::Task<void> {
                                              co_await huxerui::Delay(
                                                  std::chrono::duration<double>{0});
                                              (void)g_requests.remove(id);
                                              std::vector<RequestDraft> copy = drafts.Get();
                                              for (std::size_t i = 0; i < copy.size(); ++i) {
                                                  if (copy[i].savedId == id) {
                                                      copy.erase(copy.begin() +
                                                                 static_cast<long>(i));
                                                      if (!copy.empty() &&
                                                          activeTab.Get() >= copy.size())
                                                          activeTab = copy.size() - 1;
                                                      break;
                                                  }
                                              }
                                              drafts = copy;
                                              listVersion = listVersion.Get() + 1;
                                          });
                                      });
                },
                .danger = PopupMenuDanger::kHoverRed}};
    };

    // 分组行的菜单条目（重命名分组/删除分组）：行尾 ⋮ 按钮与右键菜单共用。
    auto groupEntries = [showRenameDialog, tasks, toast, listVersion](const db::Group& g) {
        const std::int64_t gid = g.id;
        return std::vector<PopupMenuItem>{
            PopupMenuItem{
                .label = "重命名分组",
                .on_click = [showRenameDialog, gid, currentName = g.name] {
                    showRenameDialog(currentName, [gid](const std::string& name) {
                        return g_requests.renameGroup(gid, name);
                    });
                }},
            PopupMenuItem{
                .label = "删除分组",
                .on_click = [tasks, toast, listVersion, gid] {
                    // 删除分组会重组本岛：推迟出指针事件路径
                    // （组内请求移到未分组，子分组一并删除）。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        if (const std::string err = g_requests.deleteGroup(gid); !err.empty()) {
                            toast.Show("删除分组失败: " + err);
                            co_return;
                        }
                        listVersion = listVersion.Get() + 1;
                    });
                },
                .danger = PopupMenuDanger::kHoverRed}};
    };

    // 请求行（叶子）：徽标 + 名称 + 行尾 ⋮ 菜单（重命名/删除）；depth 只影响左侧缩进。
    auto requestRow = [&](const db::SavedRequest& r, int depth) -> huxerui::View {
        const std::int64_t id = r.id;
        // 徽标：非 HTTP 的已保存请求显示类型缩写（防御；现存数据基本都是 HTTP）。
        const std::string badge = r.kind == api::RequestKind::WebSocket ? "WS"
                                  : r.kind == api::RequestKind::Tcp     ? "TCP"
                                                                        : r.method;
        return huxerui::Row {
            // 打开区：徽标 + 名称占满行宽；点击挂在整行 Row 上（见下方 .OnClick），
            // ⋮ 是最深命中节点、点击不冒泡，仍只触发它自己。
            huxerui::Row {
                huxerui::Text(badge, huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{
                        .font = methodFont,
                        // 徽标按 MethodColor 统一色表逐方法着色。
                        .foreground = MethodColor(theme, badge)})
                    .With(huxerui::Frame{.min_width = 32.0F}),
                huxerui::Text(r.name.empty() ? "（未命名）" : r.name,
                              huxerui::TextRole::Body),
            }
                .With(huxerui::Spacing(theme.spacing.extra_small),
                      huxerui::Grow(1.0F), huxerui::ClipChildren()),
            // 行尾 ⋮ 菜单（悬停显隐；重命名/删除）。锚点在按钮自己的 composable
            // 作用域里（一个 LayerAnchor 只能挂一个 View，见 RowMenuButton）。
            RowMenuButton(hoveredRow.Get() == id, requestEntries(r)),
        }
            .With(huxerui::Spacing(0.0F),
                  huxerui::Padding(huxerui::EdgeInsets{
                      .top = 4.0F, .right = 6.0F, .bottom = 4.0F,
                      .left = 6.0F + static_cast<float>(depth) * 14.0F}),
                  // 默认无底色，被选中（活跃标签对应行）或悬停（含悬停 ⋮，
                  // Hover 事件通道非独占）才显示容器底。
                  huxerui::Background(id == activeSavedId || hoveredRow.Get() == id
                                          ? theme.colors.surface_container
                                          : huxerui::Color::Transparent()),
                  huxerui::CornerRadius(theme.shapes.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  // 行自身压掉默认 Indication：悬停反馈由手工底色承担，避免叠加。
                  huxerui::Indication{})
            // 点击整行 = 打开/激活对应标签：挂整行让默认 Indication 的悬停/按压
            // 高亮覆盖整条长条（语义同分组行）。
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
            })
            // 拖拽源：鼠标按下即拖（默认距离阈值），与点击按阈值分胜负——桌面端
            // 滚动走滚轮，ScrollView 不与鼠标拖拽抢指针。preview 工厂给出拖影：
            // 拖到分组行 = 移入该分组；拖到列表空白 = 移出分组（根投放区见下）。
            .With(huxerui::DragSource(
                RequestDragPayload{id},
                [badge, name = r.name, bg = theme.colors.surface_container_high,
                 fg = theme.colors.on_surface,
                 accent = MethodColor(theme, badge)] {
                    return huxerui::Row {
                        huxerui::Text(badge, huxerui::TextRole::Label)
                            .Style(huxerui::TextStyle{
                                .font = huxerui::Font::Monospace(font_size::kCaption)
                                            .WithWeight(huxerui::FontWeight::SemiBold),
                                .foreground = accent}),
                        huxerui::Text(name.empty() ? "（未命名）" : name,
                                      huxerui::TextRole::Body),
                    }
                        .With(huxerui::Spacing(6.0F), huxerui::Padding(6.0F),
                              huxerui::Background(bg), huxerui::Foreground(fg),
                              huxerui::CornerRadius(6.0F));
                }))
            // 悬停显隐 ⋮：Enter 记行 key，Leave 时仅当仍是本行才清空（防跨行
            // 误清）。只写 hoveredRow；悬停重组靠稳定 Key 保留挂载节点（见下）。
            .On<huxerui::ViewEvents::Hover>([hoveredRow, id](const huxerui::HoverEvent& e) {
                if (e.type == huxerui::HoverEventType::Enter)
                    hoveredRow = id;
                else if (e.type == huxerui::HoverEventType::Leave && hoveredRow.Get() == id)
                    hoveredRow = 0;
            })
            // 右键菜单：条目同 ⋮ 按钮，跟随点击位置弹出；挂在行最外层容器上，
            // 命中链最深绑定生效，分组内嵌套的请求行仍弹本菜单。
            .On<huxerui::ViewEvents::ContextMenuRequested>(
                [ctxMenu, requestEntries, r](huxerui::Point pos) {
                    ShowPopupMenuAt(ctxMenu, pos, requestEntries(r));
                })
            // 稳定 Key：悬停重组（整张列表重建 rows）时按 Key 保留挂载节点
            // 与其扩展实例，避免节点替换引起的 hover 抖动。
            .Key(id);
    };

    // 分组行（内部节点）：折叠箭头 + 名称；点击切换折叠。行尾 ⋮（悬停才显示）
    // 收 重命名分组/删除分组 菜单。
    auto groupRow = [&](const db::Group& g, int depth, bool isCollapsed) -> huxerui::View {
        return huxerui::Row {
                   huxerui::Text(isCollapsed ? "▸" : "▾", huxerui::TextRole::Label)
                       .Style(huxerui::TextStyle{
                           .font = huxerui::Font::System(font_size::kCaption),
                           .foreground = theme.colors.on_surface_variant})
                       .With(huxerui::Frame{.min_width = 14.0F}),
                   huxerui::Text(g.name, huxerui::TextRole::Body)
                       .With(huxerui::ClipChildren(), huxerui::Grow(1.0F)),
                   // 行尾 ⋮ 菜单（悬停显隐；重命名分组/删除分组）。锚点在按钮自己的
                   // composable 作用域里（一个 LayerAnchor 只能挂一个 View）。
                   RowMenuButton(hoveredRow.Get() == -g.id, groupEntries(g)),
               }
                   .With(huxerui::Spacing(theme.spacing.extra_small),
                         huxerui::Padding(huxerui::EdgeInsets{
                             .top = 4.0F, .right = 6.0F, .bottom = 4.0F,
                             .left = 6.0F + static_cast<float>(depth) * 14.0F}),
                         // 悬停（含悬停 ⋮，同请求行）显示容器底。
                         huxerui::Background(hoveredRow.Get() == -g.id
                                                 ? theme.colors.surface_container
                                                 : huxerui::Color::Transparent()),
                         huxerui::CornerRadius(theme.shapes.small),
                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                         // 行自身压掉默认 Indication：悬停反馈由手工底色承担。
                         huxerui::Indication{})
                   .OnClick([collapsed, id = g.id] {
                       std::vector<std::int64_t> copy = collapsed.Get();
                       if (const auto it = std::ranges::find(copy, id); it != copy.end())
                           copy.erase(it);
                       else
                           copy.push_back(id);
                       collapsed = copy;
                   })
                   // 分组既是拖拽源（拖到别的分组 = 变其子分组），也是投放目标
                   // （接受请求/分组落入）。自身投放被谓词拒绝；成环由 store
                   // 环检测兜底（toast 报错）。落盘统一推迟出指针事件路径。
                   // 鼠标按下即拖 + 拖影 preview，理由同请求行。
                   .With(huxerui::DragSource(
                             GroupDragPayload{g.id},
                             [name = g.name, bg = theme.colors.surface_container_high,
                              fg = theme.colors.on_surface] {
                                 return huxerui::Row {
                                     huxerui::Text("▾ " + (name.empty() ? "（未命名）" : name),
                                                   huxerui::TextRole::Body),
                                 }
                                     .With(huxerui::Padding(6.0F), huxerui::Background(bg),
                                           huxerui::Foreground(fg), huxerui::CornerRadius(6.0F));
                             }),
                         huxerui::DropTarget::Accepts<RequestDragPayload>(),
                         huxerui::DropTarget::Accepts<GroupDragPayload>(
                             [gid = g.id](const GroupDragPayload& p) { return p.groupId != gid; }))
                   .On<huxerui::DropEvents<RequestDragPayload>::Dropped>(
                       [tasks, toast, listVersion, gid = g.id](const RequestDragPayload& p,
                                                               const huxerui::DropEvent&) {
                           tasks.Launch([=]() -> huxerui::Task<void> {
                               co_await huxerui::Delay(std::chrono::duration<double>{0});
                               if (const std::string err =
                                       g_requests.moveToGroup(p.requestId, gid);
                                   !err.empty()) {
                                   toast.Show("移动失败: " + err);
                                   co_return;
                               }
                               listVersion = listVersion.Get() + 1;
                           });
                       })
                   .On<huxerui::DropEvents<GroupDragPayload>::Dropped>(
                       [tasks, toast, listVersion, gid = g.id](const GroupDragPayload& p,
                                                               const huxerui::DropEvent&) {
                           tasks.Launch([=]() -> huxerui::Task<void> {
                               co_await huxerui::Delay(std::chrono::duration<double>{0});
                               if (const std::string err = g_requests.moveGroup(p.groupId, gid);
                                   !err.empty()) {
                                   toast.Show("移动失败: " + err);
                                   co_return;
                               }
                               listVersion = listVersion.Get() + 1;
                           });
                       })
                   // 悬停显隐 ⋮：理由同请求行（Enter 记 -g.id，Leave 条件清空）。
                   .On<huxerui::ViewEvents::Hover>(
                       [hoveredRow, key = -g.id](const huxerui::HoverEvent& e) {
                           if (e.type == huxerui::HoverEventType::Enter)
                               hoveredRow = key;
                           else if (e.type == huxerui::HoverEventType::Leave &&
                                    hoveredRow.Get() == key)
                               hoveredRow = 0;
                       })
                   // 右键菜单：条目同 ⋮ 按钮，跟随点击位置弹出（理由同请求行）。
                   .On<huxerui::ViewEvents::ContextMenuRequested>(
                       [ctxMenu, groupEntries, g](huxerui::Point pos) {
                           ShowPopupMenuAt(ctxMenu, pos, groupEntries(g));
                       })
                   // 稳定 Key：理由同请求行（悬停重组时保留节点与其扩展实例）。
                   // 取负与请求行 Key 区分。
                   .Key(-g.id);
    };

    // 递归展开：先子分组（未折叠才展开其子树），再本分组内的请求。
    // parentId=0 一轮即根层：顶层分组 + 未分组请求。
    std::function<void(std::int64_t, int)> emitLevel = [&](std::int64_t parentId, int depth) {
        for (const db::Group& g : groups) {
            if (g.parentId != parentId) continue;
            const bool isCollapsed = std::ranges::contains(collapsed.Get(), g.id);
            rows.push_back(groupRow(g, depth, isCollapsed));
            if (!isCollapsed) emitLevel(g.id, depth + 1);
        }
        for (const db::SavedRequest& r : saved) {
            if (r.groupId == parentId) rows.push_back(requestRow(r, depth));
        }
    };
    emitLevel(0, 0);

    if (saved.empty() && groups.empty()) {
        rows.push_back(huxerui::Text("集合为空：在右侧新建请求并保存。",
                                     huxerui::TextRole::Body)
                           .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }

    // 头部行：标题 + 圆形 "+"（弹出新建类型菜单）。不再显示项目名——
    // 顶级标签条已标识当前项目。菜单项回调在菜单层关闭后执行，不卸载被点
    // 按钮（按钮在本岛，菜单在层上），可直接写 State。
    huxerui::View island = huxerui::Column {
                               huxerui::Row {
                                   huxerui::Text("请求", huxerui::TextRole::Title)
                                       .With(huxerui::Grow(1.0F)),
                                   CircleButton("+", [menu, dialog, tasks, toast, drafts,
                                                      activeTab, listVersion, newGroupName,
                                                      newGroupMode] {
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
                                       // 新建分组：弹窗输入名称 + 模式（仅名称/路径）。
                                       entries.push_back(huxerui::MenuItem(
                                           "新建分组", [dialog, tasks, toast, listVersion,
                                                        newGroupName, newGroupMode] {
                                               newGroupName = huxerui::TextEditingValue{};
                                               newGroupMode = std::size_t{0};
                                               dialog.Show(
                                                   [tasks, toast, listVersion, newGroupName,
                                                    newGroupMode](huxerui::DialogContext ctx)
                                                       -> huxerui::View {
                                                       return DialogCard(
                                                           huxerui::Column {
                                                               huxerui::Text("新建分组",
                                                                             huxerui::TextRole::Title),
                                                               huxerui::TextField(newGroupName.Get())
                                                                   .Label("分组名称")
                                                                   .Variant(huxerui::TextFieldVariant::Outlined)
                                                                   .OnChanged([newGroupName](
                                                                          const huxerui::TextEditingValue&
                                                                              value) {
                                                                       newGroupName = value;
                                                                   }),
                                                               huxerui::SegmentedButton(
                                                                   {"仅名称", "路径"}, newGroupMode)
                                                                   .OnChanged([newGroupMode](
                                                                          std::size_t i) {
                                                                       newGroupMode = i;
                                                                   }),
                                                               huxerui::Row {
                                                                   huxerui::Button("取消")
                                                                       .OnClick([ctx] {
                                                                           ctx.Dismiss();
                                                                       }),
                                                                   huxerui::Button("创建")
                                                                       .OnClick([ctx, tasks, toast,
                                                                                 listVersion,
                                                                                 newGroupName,
                                                                                 newGroupMode] {
                                                                           if (newGroupName.Get()
                                                                                   .text.empty()) {
                                                                               toast.Show(
                                                                                   "分组名称不能为空");
                                                                               return;
                                                                           }
                                                                           ctx.Dismiss();
                                                                           // 建组重组左岛：推迟出指针事件路径
                                                                           tasks.Launch([=]()
                                                                                   -> huxerui::Task<
                                                                                       void> {
                                                                               co_await huxerui::Delay(
                                                                                   std::chrono::duration<
                                                                                       double>{0});
                                                                               if (const std::string
                                                                                       err = g_requests
                                                                                           .createGroup(
                                                                                               newGroupName
                                                                                                   .Get()
                                                                                                   .text,
                                                                                               newGroupMode
                                                                                                           .Get() ==
                                                                                                       1
                                                                                                   ? db::GroupMode::
                                                                                                       Path
                                                                                                   : db::GroupMode::
                                                                                                       Name);
                                                                                   !err.empty()) {
                                                                                   toast.Show(
                                                                                       "新建分组失败: " +
                                                                                       err);
                                                                                   co_return;
                                                                               }
                                                                               listVersion =
                                                                                   listVersion.Get() +
                                                                                   1;
                                                                           });
                                                                       }),
                                                               }
                                                                   .With(huxerui::MainAlign(
                                                                       huxerui::MainAxisAlignment::
                                                                           SpaceBetween)),
                                                           }
                                                               .With(huxerui::Spacing(12.0F),
                                                                     huxerui::Frame{.width =
                                                                                        320.0F},
                                                                     huxerui::CrossAlign(
                                                                         huxerui::CrossAxisAlignment::
                                                                             Stretch)));
                                                   },
                                                   huxerui::DialogOptions{});
                                           }));
                                       menu.Show(std::move(entries),
                                                 huxerui::MenuOptions{
                                                     .placement = {huxerui::AnchorSide::Below,
                                                                   huxerui::AnchorAlignment::Start}});
                                   }).With(menu.Anchor()),
                               }
                                   .With(huxerui::CrossAlign(
                                       huxerui::CrossAxisAlignment::Center)),
                               // 根投放区：落到列表空白/非分组行上 = 移到根目录
                               // （请求移出分组，分组回到顶层）。
                               huxerui::ScrollView{
                                   huxerui::Column(std::move(rows))
                                       .With(huxerui::Spacing(theme.spacing.small))
                                       .With(huxerui::DropTarget::Accepts<RequestDragPayload>(),
                                             huxerui::DropTarget::Accepts<GroupDragPayload>())
                                       .On<huxerui::DropEvents<RequestDragPayload>::Dropped>(
                                           [tasks, toast, listVersion](
                                               const RequestDragPayload& p,
                                               const huxerui::DropEvent&) {
                                               tasks.Launch([=]() -> huxerui::Task<void> {
                                                   co_await huxerui::Delay(
                                                       std::chrono::duration<double>{0});
                                                   if (const std::string err =
                                                           g_requests.moveToGroup(p.requestId, 0);
                                                       !err.empty()) {
                                                       toast.Show("移动失败: " + err);
                                                       co_return;
                                                   }
                                                   listVersion = listVersion.Get() + 1;
                                               });
                                           })
                                       .On<huxerui::DropEvents<GroupDragPayload>::Dropped>(
                                           [tasks, toast, listVersion](
                                               const GroupDragPayload& p,
                                               const huxerui::DropEvent&) {
                                               tasks.Launch([=]() -> huxerui::Task<void> {
                                                   co_await huxerui::Delay(
                                                       std::chrono::duration<double>{0});
                                                   if (const std::string err =
                                                           g_requests.moveGroup(p.groupId, 0);
                                                       !err.empty()) {
                                                       toast.Show("移动失败: " + err);
                                                       co_return;
                                                   }
                                                   listVersion = listVersion.Get() + 1;
                                               });
                                           })}
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

    // 未打开项目的兜底已删：主页整宽覆盖侧栏后本页在 opened==0 时不可达。
    (void)opened;

    // 内部标签页：每个打开的请求一个草稿；响应区状态为页面级（单引擎）。
    auto openDrafts = huxerui::UseState<std::vector<RequestDraft>>({});
    auto activeTab = huxerui::UseState<std::size_t>(0);
    auto listVersion = huxerui::UseState(0);
    // 环境版本：环境选择/CRUD/弹窗保存后 bump，标签条与环境弹窗按它重读 store。
    auto envVersion = huxerui::UseState(0);
    // 编辑器子页：0=调试 1=文档 2=测试用例 3=Mock（仅 HTTP 草稿用；非调试页时
    // 响应下岛让位，编辑器占满右岛）。
    auto editorPage = huxerui::UseState<std::size_t>(0);
    auto inFlight = huxerui::UseState(false);
    auto responseBody = huxerui::UseState(std::string{"（尚未发送请求）"});
    auto responseHeaders = huxerui::UseState<std::vector<std::string>>({});
    auto responseCookies = huxerui::UseState<std::vector<std::string>>({});

    // 响应式：Compact（<600pt）改上下堆叠，Medium/Expanded 保持左右双岛。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    const std::vector<RequestDraft> snapshot = openDrafts.Get();
    const std::size_t current =
        snapshot.empty() ? 0 : std::min(activeTab.Get(), snapshot.size() - 1);
    const int currentKind = snapshot.empty() ? 0 : snapshot[current].kind;
    // 右岛内容 Key：kind + 标签下标组合，切标签/切类型时正确重组。
    const std::int64_t contentKey = static_cast<std::int64_t>(current) * 4 + currentKind;

    // 右侧按活跃草稿类型分派：
    // - HTTP：拆上下两岛——上岛 = 标签条 + 编辑器（Grow 3），下岛 = 响应区
    //   （Grow 2）；编辑器固定部分（名称/操作栏/分区切换/Body 类型行）不滚动，
    //   分区内容与响应各自内滚（垂直滚动条）。
    // - WS/TCP：直接嵌入整页组件（内部自带 ScrollView/事件泵，勿再套 ScrollView，
    //   避免同轴嵌套滚动）；固定 kUid=1 引擎会话，同类型标签共享同一条连接。
    // - 无打开草稿：单岛空状态。
    huxerui::View rightArea = huxerui::Column{};
    if (snapshot.empty()) {
        rightArea = huxerui::Column {
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
                              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                              huxerui::Background(theme.colors.surface_container_low),
                              huxerui::CornerRadius(theme.shapes.large),
                              huxerui::Padding(theme.spacing.medium),
                              huxerui::Grow(1.0F));
    } else if (currentKind == 1 || currentKind == 2) {
        huxerui::View content = currentKind == 1
                                    ? WebSocketPage().With(huxerui::Grow(1.0F)).Key(contentKey)
                                    : TcpPage().With(huxerui::Grow(1.0F)).Key(contentKey);
        rightArea = huxerui::Column {
                        RequestTabStrip(openDrafts, activeTab, envVersion),
                        std::move(content),
                    }
                        .With(huxerui::Spacing(theme.spacing.medium),
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                              huxerui::Background(theme.colors.surface_container_low),
                              huxerui::CornerRadius(theme.shapes.large),
                              huxerui::Padding(theme.spacing.medium),
                              huxerui::Grow(1.0F));
    } else {
        // 非调试子页（文档/测试用例/Mock）：编辑器占满右岛，响应下岛让位。
        const bool debugging = editorPage.Get() == 0;
        std::vector<huxerui::View> islands;
        islands.push_back(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion),
                RequestEditor(openDrafts, current, activeTab, listVersion,
                              inFlight, responseBody, responseHeaders,
                              responseCookies, envVersion, editorPage)
                    .Key(contentKey)
                    .With(huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Background(theme.colors.surface_container_low),
                      huxerui::CornerRadius(theme.shapes.large),
                      huxerui::Padding(theme.spacing.medium),
                      huxerui::Grow(debugging ? 3.0F : 1.0F)));
        if (debugging) {
            islands.push_back(
                huxerui::Column {
                    ResponseArea(responseBody, responseHeaders, responseCookies, theme)
                        .With(huxerui::Grow(1.0F)),
                }
                    .With(huxerui::Spacing(theme.spacing.small),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                          huxerui::Background(theme.colors.surface_container_low),
                          huxerui::CornerRadius(theme.shapes.large),
                          huxerui::Padding(theme.spacing.medium),
                          huxerui::Grow(2.0F)));
        }
        rightArea = huxerui::Column(std::move(islands))
                        .With(huxerui::Spacing(theme.spacing.small),
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                              huxerui::Grow(1.0F));
    }

    if (compact) {
        // Compact：上 = 请求列表（限高 220、宽度撑满），下 = 编辑区（撑满剩余）。
        return huxerui::Column {
                   RequestListIsland(openDrafts, activeTab, listVersion, true),
                   std::move(rightArea),
               }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return huxerui::Row {
               RequestListIsland(openDrafts, activeTab, listVersion, false),
               std::move(rightArea),
           }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
