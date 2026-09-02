// request_page.cpp — 请求工作区：左岛 = 当前项目的请求列表；右侧 HTTP 拆上下两岛
// （上 = 标签条 + 编辑器，下 = 响应区；WS/TCP 自包含整页仍单岛；gRPC 单岛占位）。
// 发送走 store 持有的 curl 引擎（api::ApiEngine 抽象：send 纯入队，UI 协程
// PollWhile 轮询 takeResponse 取回结果，回 UI 线程写 State 并落历史）；
// 保存落到当前项目集合并刷新左岛列表。
// P1-C1（2026-09-02）：左岛请求集合树（RequestListIsland）已拆至 request_list.cpp；
// 导入接口弹窗（ApiImportDialogContent）提升为外部链接（ui.h 声明）。本文件保留
// RequestPage 编排与右岛各岛（标签条 / 编辑器 / 响应 / 环境 / Body）。
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
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ui.h"
#include "draft.h"
#include "api_import.h"
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
                    ? huxerui::View{huxerui::Row{}.With(
                          huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                    : AppIconButton("✕", "删除此行", [tasks, rows, i, onChanged] {
                        // 删除会移除本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<KvRow> copy = rows;
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            onChanged(std::move(copy));
                        });
                    }, AppIconButtonShape::Bare),
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
                AppIconButton("✎", "重命名环境", [dialog, tasks, toast, renameValue, envVersion, id,
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
                    }, AppIconButtonShape::Bare),
                // ✕ 删除：危险确认框（共享 helper，确认按钮染红）；删除重组本弹窗 → 推迟。
                AppIconButton("✕", "删除环境", [dialog, tasks, selectedId, envVersion, id,
                                                      name = e.name] {
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
                    }, AppIconButtonShape::Bare),
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
                    // 独立浮动 "+"：保持 Circular + compact 档（28pt），视觉不变。
                    AppIconButton("+", "新建环境", [tasks, toast, selectedId, envVersion] {
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
                    }, AppIconButtonShape::Circular, 28.0F, /*accent=*/false),
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
    auto envSearching = huxerui::UseState(false);
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
                AppIconButton("✕", "关闭请求标签", [tasks, drafts, activeTab, i] {
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
                    }, AppIconButtonShape::Bare, 28.0F, false, chipHovered)
                    .With(huxerui::Opacity(chipHovered ? 1.0F : 0.0F)),
            }
                .With(huxerui::Spacing(0.0F), huxerui::Background(fill),
                      huxerui::CornerRadius(theme.shapes.small),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(4.0F, 2.0F)),
                      huxerui::Frame{.width = kChipDragWidth, .height = 28.0F},
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
    chips.push_back(AppIconButton("+", "新建请求标签", [drafts, activeTab] {
                            std::vector<RequestDraft> copy = drafts.Get();
                            copy.push_back(RequestDraft{});
                            drafts = copy;
                            activeTab = copy.size() - 1;
                        }, AppIconButtonShape::Circular));

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
                          huxerui::Frame{.width = kChipDragWidth, .height = 28.0F},
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
    // Spacing(0)，左侧环境选择扁平触发器（环境名 + 统一无尾箭头弹自绘下拉，选中项
    // 深色填充底色、无对钩；保持自绘而非官方 Select——Select 触发器自带描边
    // 外观，塞不进共用外框），中间 1pt 竖分隔线，右侧 ☰ 为 Bare AppIconButton
    // （环境配置弹窗入口；常态透明、hover 才显统一圆角方形底——与左侧触发器
    // 同一"框内扁平段"视觉）。外层 Row 交叉轴 Stretch 让分隔线拉满全高，
    // 左触发器与右侧按钮各自 CrossAlign(Center) 垂直居中。
    // 外框圆角取几何令牌 control_radius（P1-A3：不再散落 shapes.small 字面量）。
    const IslandTheme islands = ResolveIslandTheme(theme);
    struct EnvChoice {
        std::int64_t id = 0;
        std::string label;
    };
    std::vector<EnvChoice> allChoices{{.id = 0, .label = "无"}};
    for (const db::Environment& env : envs)
        allChoices.push_back(EnvChoice{.id = env.id,
                                       .label = env.name.empty() ? "（未命名）" : env.name});

    // 官方 ComboBox：受控输入只用于搜索过滤，选中建议才真正切换环境。
    // 初始化为当前环境名称；选择后同步回写完整 TextEditingValue。
    auto envQuery = huxerui::UseState(
        huxerui::TextEditingValue{envNames.at(currentEnv)});
    const std::string needle = envQuery.Get().text;
    auto containsFolded = [](const std::string& value, const std::string& query) {
        std::string lhs = value;
        std::string rhs = query;
        std::ranges::transform(lhs, lhs.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::ranges::transform(rhs, rhs.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return rhs.empty() || lhs.find(rhs) != std::string::npos;
    };
    std::vector<EnvChoice> matchingChoices;
    for (const EnvChoice& choice : allChoices)
        if (containsFolded(choice.label, needle)) matchingChoices.push_back(choice);

    huxerui::View envSearch =
        huxerui::ComboBox(
            envQuery, matchingChoices,
            [](const EnvChoice& choice) { return choice.label; },
            [](const EnvChoice& choice) { return huxerui::Text(choice.label); })
            .Placeholder("搜索环境")
            .Variant(huxerui::TextFieldVariant::Standard)
            .EmptyContent([] {
                return huxerui::Text("没有匹配的环境", huxerui::TextRole::Label)
                    .With(huxerui::Padding(10.0F));
            })
            .OnChanged([envQuery](const huxerui::TextEditingValue& value) {
                envQuery = value;
            })
            .OnSelected([tasks, envQuery, envSearching, matchingChoices, envVersion, toast](
                            std::size_t selected, const huxerui::TextEditingValue& value) {
                if (selected >= matchingChoices.size()) return;
                if (const std::string err =
                        g_requests.selectEnv(matchingChoices[selected].id);
                    !err.empty()) {
                    toast.Show("切换环境失败: " + err);
                    return;
                }
                envQuery = value;
                envVersion = envVersion.Get() + 1;
                tasks.Launch([envSearching]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    envSearching = false;
                });
            })
            // 搜索态原位替换紧凑按钮，不能改变标题栏的占位尺寸。
            // 不再用失焦关闭：只有完成选择才恢复为紧凑按钮，避免点击候选项时闪烁。
            .With(huxerui::Frame{.width = 136.0F, .height = islands.control_height},
                  huxerui::ClipChildren());

    huxerui::View envCompact = huxerui::Row {
        huxerui::Row{huxerui::Text(envNames.at(currentEnv), huxerui::TextRole::Body)
                          .With(huxerui::Foreground(theme.colors.on_surface),
                                huxerui::ClipChildren())}
            .With(huxerui::Frame{.width = 112.0F, .height = islands.control_height},
                  huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 0.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Row{huxerui::Image(app::images::chevron_down)
                          .Fit(huxerui::ImageFit::Contain)
                          .Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center)
                          .Tint(theme.colors.on_surface_variant)
                          .With(huxerui::Frame{.width = 12.0F, .height = 12.0F})}
            .With(huxerui::Frame{.width = 24.0F, .height = islands.control_height},
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Focusable(false)),
    }.With(huxerui::Frame{.width = 136.0F, .height = islands.control_height},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
           huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                               .label = "搜索并选择环境"},
           huxerui::Focusable(true))
        .OnClick([tasks, envSearching, envQuery, currentName = envNames.at(currentEnv)] {
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                // 搜索框的默认值必须是点击前正在使用的环境，而不是空查询。
                envQuery = huxerui::TextEditingValue{currentName};
                envSearching = true;
            });
        });

    huxerui::View envTrigger = envSearching.Get() ? std::move(envSearch)
                                                  : std::move(envCompact);

    // ☰：环境配置弹窗（自定义内容层，DialogFactory）。P1-A4 收口：原
    // Text("☰")+Padding 热区不足 28pt 且无语义标签/Tooltip，迁为统一 Bare
    // AppIconButton——semanticLabel"环境配置"兼作可访问名称与 Tooltip，hover/
    // press indication 覆盖整个 28×28 命中区。外包垂直居中容器：外层 Row 交叉
    // 轴 Stretch 会把固定高子项拉到行高，包一层 CrossAlign(Center) 保住
    // 28×28 命中区与方形 hover 底。
    huxerui::View envSettingsTrigger =
        huxerui::Row {
            AppIconButton("☰", "环境配置",
                          [dialog, envVersion] {
                              dialog.Show(
                                  [envVersion](huxerui::DialogContext ctx) -> huxerui::View {
                                      return EnvironmentDialog(ctx, envVersion);
                                  },
                                  huxerui::DialogOptions{});
                          },
                          AppIconButtonShape::Bare, 28.0F),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));

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
                  huxerui::CornerRadius(islands.control_radius), huxerui::ClipChildren(),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::ClipChildren());
}

// 编辑器子页"测试用例"/"Mock"实现在 testcase_page.cpp / mock_page.cpp
// （声明在 ui.h）；居中空态样式各自文件内自带。

// 右上岛编辑区：选择行（调试/文档/测试用例/Mock + 短名称修改框）+ 调试页的
// 方法/URL/发送/保存/⋮ 操作栏 + Params/Headers/Cookies/Body 分区。固定部分
// （选择行/操作栏/分区切换/Body 类型行）不滚动；分区内容（KV 表或 Body 编辑器）
// 在内部 ScrollView 里滚动并带垂直滚动条——所有 Body 类型输入框（含 Form 类
// KV 表）都有滚动条。响应区已拆为独立下岛（见 RequestPage，仅调试页显示）。
huxerui::View SplitActionButton(
    std::string label, std::function<void(bool)> onAction, std::string alternateLabel,
    bool alternateEnabled);

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
    std::shared_ptr<huxerui::FilePicker> filePicker;
    std::shared_ptr<huxerui::FileSystem> fileSystem;
    try {
        filePicker = huxerui::UseService<huxerui::FilePicker>();
        fileSystem = huxerui::UseService<huxerui::FileSystem>();
    } catch (const std::exception&) {
        filePicker = nullptr;
        fileSystem = nullptr;
    }
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

    // “发送并下载”：传输完成后把原始响应体写入应用临时目录，再交给系统保存选择器。
    // 临时文件无论保存/取消都会清理；文件服务不可用时给出明确提示。
    const auto downloadResponse =
        [filePicker, fileSystem, toast](std::string body,
                                        std::string suggestedStem) -> huxerui::Task<void> {
            if (!filePicker || !fileSystem || !filePicker->CanSaveFiles()) {
                toast.Show("当前平台不支持保存响应文件");
                co_return;
            }
            for (char& ch : suggestedStem) {
                if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
                    ch == '"' || ch == '<' || ch == '>' || ch == '|')
                    ch = '_';
            }
            if (suggestedStem.empty()) suggestedStem = "response";
            const huxerui::File temporary = fileSystem->Directories().temporary_directory.Child(
                "apitab-response-" + std::to_string(NextDraftUid()) + ".txt");
            if (!co_await temporary.WriteStringAsync(std::move(body))) {
                toast.Show("准备响应下载文件失败");
                co_return;
            }
            const bool saved = co_await filePicker->SaveFileAsync(
                temporary,
                huxerui::SaveFileOptions{
                    .suggested_name = suggestedStem + ".txt",
                    .filter = huxerui::FilePickerFilter{.name = "响应文件",
                                                        .extensions = {"txt", "json"}}});
            (void)co_await temporary.DeleteAsync();
            toast.Show(saved ? "响应已下载" : "已取消下载");
        };

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

    // 保存入口共用一条持久化路径。asCase=true 时，从最近成功响应捕获 HTTP 状态码，
    // 追加成测试用例后与请求整体落库；当前测试用例模型表达响应断言，不复制请求快照。
    const auto saveDraft = [=](bool asCase) {
        const std::vector<RequestDraft> current = drafts.Get();
        if (index >= current.size()) return;
        RequestDraft draft = current[index];
        if (draft.name.text.empty()) {
            toast.Show("请先在“名称”里填写请求名");
            return;
        }

        if (asCase) {
            // 正常与 Mock 响应头分别是 "HTTP …" / "MOCK · HTTP …"，都从 HTTP
            // 标记后读取首个整数。失败、取消、在途或尚未发送均不会生成空用例。
            const std::string response = responseBody.Get();
            const std::size_t marker = response.find("HTTP ");
            if (marker == std::string::npos) {
                toast.Show("请先发送请求并获得成功响应");
                return;
            }
            const std::size_t statusBegin = marker + 5;
            const std::size_t statusEnd = response.find_first_not_of("0123456789", statusBegin);
            int status = 0;
            const char* begin = response.data() + statusBegin;
            const char* end = response.data() +
                              (statusEnd == std::string::npos ? response.size() : statusEnd);
            const auto parsed = std::from_chars(begin, end, status);
            if (parsed.ec != std::errc{} || status <= 0) {
                toast.Show("当前响应状态不可用于创建用例");
                return;
            }
            TestCaseDraft captured;
            captured.name = huxerui::TextEditingValue{draft.name.text + " · 状态 " +
                                                       std::to_string(status)};
            captured.expectStatus = huxerui::TextEditingValue{std::to_string(status)};
            draft.cases.push_back(std::move(captured));
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
        for (std::size_t i = 0; i < saved.bodyContents.size(); ++i)
            saved.bodyContents[i].text = draft.bodies[i].text;
        const std::size_t bodyIndex = static_cast<std::size_t>(spec.bodyKind);
        if (bodyIndex < saved.bodyContents.size())
            saved.bodyContents[bodyIndex].fields = spec.bodyFields;
        for (const TestCaseDraft& c : draft.cases) saved.testCases.push_back(CaseToDb(c));
        saved.mock = MockToDb(draft.mock);
        if (const std::string err = g_requests.save(saved); !err.empty()) {
            toast.Show("保存失败: " + err);
            return;
        }
        toast.Show(asCase ? "已保存当前状态为用例" : "已保存到集合");
        // 新草稿拿到持久化 id；捕获用例时同步回写新增项；左岛列表刷新。
        MutateDraft(drafts, index, [&](RequestDraft& d) {
            d.savedId = saved.id;
            if (asCase) d.cases = draft.cases;
        });
        listVersion = listVersion.Get() + 1;
    };

    // 操作栏：方法+URL 合并控件（占满）+ 发送/取消 + 保存⌄ + ⋮（删除）。
    // Compact 窄宽度契约：Row 布局里非 Grow 子元素按自然宽度保留（HuxerUI
    // 只把剩余宽度以紧约束分给 Grow 子元素，不足时钳到 0），故发送/取消/保存
    // 永远完整可见；MethodUrlBar 是唯一 Grow 子元素且自带 ClipChildren，宽度
    // 不足时先收缩 URL 字段、再裁剪 baseUrl 段，不会遮挡右侧动作。
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
            SplitActionButton(inFlight.Get() ? "取消" : "发送", [=](bool download) {
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
                        if (download)
                            co_await downloadResponse(view.body, current[index].name.text);
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
                        if (download && view.ok)
                            co_await downloadResponse(view.body, current[index].name.text);
                    });
                });
            }, "发送并下载", !inFlight.Get()),
            SplitActionButton("保存", saveDraft, "保存当前状态为用例", true),
            // ⋮ 溢出菜单：删除当前请求条（自绘 PopupMenu，删除项 hover 才显红；
            // 点击先弹危险确认框；已保存的连集合一起删，并关掉本标签）。
            // 删除会卸载本编辑器 → 确认回调里推迟出指针事件路径。
            // OverflowButton = "⋮"/"更多操作" 语义（Bare 28pt，工具栏溢出动作），
            // 回调体与菜单内容保持原样。
            OverflowButton([overflow, dialog, tasks, drafts, activeTab, listVersion, index] {
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
                })
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


// 分裂动作按钮：左区执行默认动作，右侧固定 24pt 的无尾箭头在 hover-enter 或点击时
// 展开替代动作。箭头使用 12×12 SVG 并在 32pt 热区内双轴居中，不受文字基线影响。
[[huxerui::composable]] huxerui::View SplitActionButton(
    std::string label, std::function<void(bool)> onAction, std::string alternateLabel,
    bool alternateEnabled) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto popup = huxerui::UsePopup();
    struct HoverMenuState {
        huxerui::LayerId layer = 0;
    };
    // 非响应式状态只记录当前层；Enter 只 Show 一次，Leave 立即 Dismiss，按钮不重组。
    auto hover = huxerui::UseState(std::make_shared<HoverMenuState>()).Get();
    auto showAlternates = [popup, alternateLabel, onAction, alternateEnabled, hover] {
        if (!alternateEnabled) return;
        if (hover->layer != 0) return; // 同一次 hover 只挂一层，避免重复 Show 闪烁
        hover->layer = ShowHoverAppMenu(
            popup,
            {AppMenuItem{.label = alternateLabel,
                         .onClick = [onAction, hover] {
                             hover->layer = 0;
                             onAction(true);
                         }}},
            [](bool) {},
            huxerui::PopupOptions{
                .placement = {huxerui::AnchorSide::Below,
                              huxerui::AnchorAlignment::End}});
    };
    return huxerui::Row {
        huxerui::Row{huxerui::Text(std::move(label), huxerui::TextRole::Label)
                          .With(huxerui::Foreground(theme.colors.on_primary))}
            .With(huxerui::Frame{.width = 60.0F, .height = islands.control_height},
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Focusable(true))
            .OnClick([onAction] { onAction(false); }),
        huxerui::Row{huxerui::Image(app::images::chevron_down)
                          .Fit(huxerui::ImageFit::Contain)
                          .Align(huxerui::HorizontalAlignment::Center,
                                 huxerui::VerticalAlignment::Center)
                          .Tint(theme.colors.on_primary)
                          .With(huxerui::Frame{.width = 12.0F, .height = 12.0F})}
            .With(huxerui::Frame{.width = 24.0F, .height = islands.control_height},
                  huxerui::Background(huxerui::Color::Transparent()),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Focusable(true), huxerui::Enabled(alternateEnabled), popup.Anchor(),
                  huxerui::Opacity(alternateEnabled ? 1.0F : 0.4F))
            .OnClick(showAlternates)
            .On<huxerui::ViewEvents::Hover>(
                [popup, hover, showAlternates](const huxerui::HoverEvent& event) {
                    if (event.type == huxerui::HoverEventType::Enter) {
                        showAlternates();
                    } else if (event.type == huxerui::HoverEventType::Leave) {
                        if (hover->layer != 0) popup.Dismiss(hover->layer);
                        hover->layer = 0;
                    }
                }),
    }.With(huxerui::Background(theme.colors.primary),
           huxerui::CornerRadius(islands.control_radius), huxerui::ClipChildren(),
           // 分裂按钮必须是固定自然宽度；若不钳制，外层 Row 会把它当可扩张
           // 容器吞掉操作栏余量，后面的“保存”被挤出屏幕。URL 栏才是唯一 Grow 项。
           huxerui::Frame{.width = 84.0F, .height = islands.control_height},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

// ---- 导入接口弹窗（"+" 菜单 → 导入接口…）----

// api_import.h 契约的 bodyKind 字符串 → 草稿/db 的 BodyKind 下标：
// ""=0 None、json=1、text=2、xml=5、graphql=6；form=3（FormUrlEncoded——
// 导入的示例体是纯文本，url-encoded 形态最贴近，Form-Data(4) 的字段表
// 无法从一段文本还原）；其余按 text。
std::size_t ImportedBodyKindIndex(const std::string& kind) {
    if (kind == "json") return 1;
    if (kind == "xml") return 5;
    if (kind == "form") return 3;
    if (kind == "graphql") return 6;
    if (kind.empty()) return 0;
    return 2;
}


} // namespace

// 导入接口弹窗内容（P1-C1 起具外部链接，供请求集合树左岛“+”菜单调用；
// 原在匿名命名空间内，拆分后经 ui.h 声明跨 TU 可见。依赖的 ImportedBodyKindIndex/
// InferKvType 仍为上方匿名命名空间私有实现，本函数同 TU 内引用不受影响。）
// 导入接口对话框（DialogCard 布局，同「环境配置弹窗」的自定义内容层写法）：
// 文件来源用 SDK 公开文件选择服务 huxerui::FilePicker（UseService 取句柄 →
// OpenFileAsync → FileReference，Linux/macOS/Windows 与预编译 SDK 均有实现）。
// FileReference 是平台授予的读取能力、不暴露本地路径，故全文经
// ReadStringAsync() 读入（而非 std::ifstream）；若服务不可用（未安装——老版
// 预编译 SDK 回落，或 CanOpenFiles false）则退化为多行 TextField 粘贴。
// 解析成功后给出
// 「标题：N 个接口」+ 前几条目录/接口预览 + 「导入」；失败显示错误文本，
// 可重选文件重试。导入执行为纯同步 store 调用，整体推迟出指针事件路径
// （tasks.Launch + Delay(0)，CLAUDE.md 约定 6）。
[[huxerui::composable]] huxerui::View ApiImportDialogContent(huxerui::DialogContext ctx,
                                                             huxerui::State<int> listVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // UseService 未安装时抛 logic_error（老版运行时）：兜住转粘贴退化路径。
    std::shared_ptr<huxerui::FilePicker> picker;
    try {
        picker = huxerui::UseService<huxerui::FilePicker>();
    } catch (const std::exception&) {
        picker = nullptr;
    }
    const bool canPick = picker != nullptr && picker->CanOpenFiles();
    // 导入状态：fileName = 已选文件名（文件选择器不返回路径，展示名即可辨识）；
    // parsed = 解析结果（shared_ptr，空 = 尚未成功解析）；importError = 错误文本。
    auto fileName = huxerui::UseState(std::string{});
    auto parsed = huxerui::UseState(std::shared_ptr<ImportedApi>{});
    auto importError = huxerui::UseState(std::string{});
    auto pasteValue = huxerui::UseState(huxerui::TextEditingValue{});

    // 解析一段文件全文：成功入 parsed，失败入 importError（互相清零）。
    auto parseText = [parsed, importError](const std::string& text) {
        auto api = std::make_shared<ImportedApi>();
        std::string err;
        if (!ParseApiFile(text, *api, err)) {
            parsed = nullptr;
            importError = err;
            return;
        }
        importError = std::string{};
        parsed = api;
    };

    std::vector<huxerui::View> children{
        huxerui::Text("导入接口", huxerui::TextRole::Title),
    };
    if (canPick) {
        children.push_back(huxerui::Row {
            huxerui::Button("选择文件…").OnClick([tasks, picker, parseText, fileName, parsed,
                                                  importError] {
                tasks.Launch([picker, parseText, fileName, parsed,
                              importError]() -> huxerui::Task<void> {
                    const auto ref = co_await picker->OpenFileAsync(
                        huxerui::FilePickerFilter{.name = "接口文件 (JSON)",
                                                  .extensions = {"json", "yaml", "yml"}});
                    if (!ref.has_value()) co_return; // 用户取消
                    auto read = co_await ref->ReadStringAsync();
                    if (!read.Succeeded()) {
                        parsed = nullptr;
                        importError = "读取文件失败: " + read.Error().message;
                        co_return;
                    }
                    fileName = ref->Name();
                    parseText(read.Value());
                });
            }),
            huxerui::Text(fileName.Get().empty() ? "未选择文件" : fileName.Get(),
                          huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant),
                      huxerui::Grow(1.0F), huxerui::ClipChildren()),
        }
                      .With(huxerui::Spacing(theme.spacing.small),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    } else {
        // 无文件选择能力（如受限平台）：退化为粘贴文件全文再解析。
        children.push_back(huxerui::TextField(pasteValue.Get())
                               .Label("文件内容（JSON）")
                               .Placeholder("粘贴 Swagger/OpenAPI 或 Postman Collection 全文")
                               .Variant(huxerui::TextFieldVariant::Outlined)
                               .LineLimits(huxerui::TextFieldLineLimits::MultiLine(6))
                               .OnChanged([pasteValue](const huxerui::TextEditingValue& value) {
                                   pasteValue = value;
                               }));
        children.push_back(huxerui::Row {
            huxerui::Button("解析粘贴内容").OnClick([pasteValue, parseText] {
                parseText(pasteValue.Get().text);
            }),
        });
    }
    if (!importError.Get().empty()) {
        children.push_back(huxerui::Text(importError.Get(), huxerui::TextRole::Body)
                               .With(huxerui::Foreground(theme.colors.error)));
    }
    // 预览：标题 + 接口总数 + 前 6 条「目录 · 方法 URL — 名称」。
    if (const auto api = parsed.Get(); api != nullptr) {
        children.push_back(huxerui::Text(
            api->title + "：" + std::to_string(api->operations.size()) + " 个接口",
            huxerui::TextRole::Body));
        std::vector<huxerui::View> lines;
        for (std::size_t i = 0; i < api->operations.size() && i < 6; ++i) {
            const ImportedOperation& op = api->operations[i];
            std::string dir;
            for (const std::string& seg : op.dirChain) {
                if (!dir.empty()) dir += '/';
                dir += seg;
            }
            std::string line = (dir.empty() ? "（根）" : dir) + " · " + op.method + " " + op.url;
            if (!op.name.empty()) line += " — " + op.name;
            lines.push_back(huxerui::Text(std::move(line), huxerui::TextRole::Label)
                                .With(huxerui::Foreground(theme.colors.on_surface_variant),
                                      huxerui::ClipChildren()));
        }
        if (api->operations.size() > 6) {
            lines.push_back(huxerui::Text(
                "…（其余 " + std::to_string(api->operations.size() - 6) + " 条略）",
                huxerui::TextRole::Label)
                                .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        }
        children.push_back(huxerui::Column(std::move(lines))
                               .With(huxerui::Spacing(2.0F),
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    }
    children.push_back(huxerui::Row {
        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        huxerui::Button("导入").OnClick([ctx, tasks, toast, listVersion, parsed] {
            const auto api = parsed.Get();
            if (api == nullptr) {
                toast.Show("请先选择文件并成功解析");
                return;
            }
            ctx.Dismiss();
            // 导入重组左岛列表：推迟出指针事件路径（约定 6）。dirChain 逐级
            // find-or-create 分组（Name 模式，仅组织作用、不参与 URL），每条
            // op 组装 db::SavedRequest 落库；遇错即停并 toast。
            tasks.Launch([api, toast, listVersion]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                std::size_t count = 0;
                for (const ImportedOperation& op : api->operations) {
                    std::int64_t parent = 0;
                    bool failed = false;
                    for (const std::string& seg : op.dirChain) {
                        auto findChild = [&](std::int64_t parentId) {
                            std::int64_t gid = 0;
                            for (const db::Group& g : g_requests.groups()) {
                                if (g.parentId == parentId && g.name == seg) {
                                    gid = g.id;
                                    break;
                                }
                            }
                            return gid;
                        };
                        std::int64_t gid = findChild(parent);
                        if (gid == 0) {
                            if (const std::string err = g_requests.createGroup(
                                    seg, db::GroupMode::Name, parent);
                                !err.empty()) {
                                toast.Show("导入失败: " + err);
                                failed = true;
                                break;
                            }
                            gid = findChild(parent); // store 建后 reload，按名回查 id
                            if (gid == 0) {
                                toast.Show("导入失败: 新建目录回查不到");
                                failed = true;
                                break;
                            }
                        }
                        parent = gid;
                    }
                    if (failed) co_return;
                    db::SavedRequest rec;
                    rec.groupId = parent;
                    rec.name = op.name.empty() ? op.method + " " + op.url : op.name;
                    rec.method = op.method; // 原样字符串（保存/发送接受任意方法名）
                    rec.url = op.url;       // fullUrl 原样（含 scheme，finalizeSpec 不再
                                            // 拼环境）；否则为 path 相对
                    for (const ImportedParam& p : op.params) {
                        rec.params.push_back(api::KeyValue{.key = p.key, .value = p.value,
                                                           .enabled = true,
                                                           .type = InferKvType(p.value),
                                                           .remark = p.remark});
                    }
                    for (const ImportedParam& h : op.headers) {
                        rec.headers.push_back(api::KeyValue{.key = h.key, .value = h.value,
                                                            .enabled = true,
                                                            .type = InferKvType(h.value),
                                                            .remark = h.remark});
                    }
                    const std::size_t bk = ImportedBodyKindIndex(op.bodyKind);
                    rec.bodyKind = static_cast<api::BodyKind>(bk);
                    rec.body = op.body; // 兼容字段：当前类型文本
                    if (bk < rec.bodyContents.size()) rec.bodyContents[bk].text = op.body;
                    if (const std::string err = g_requests.save(rec); !err.empty()) {
                        toast.Show("导入失败: " + err);
                        co_return;
                    }
                    ++count;
                }
                toast.Show(std::format("已导入 {} 个接口", count));
                listVersion = listVersion.Get() + 1;
            });
        }),
    }
                      .With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)));
    return DialogCard(huxerui::Column(std::move(children))
                          .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 520.0F},
                                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

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
    // - gRPC（kind=3）：引擎未实现——整页占位（仅标签条 + 提示文案，无操作栏、
    //   无分区、不可保存/发送；gRPC 草稿不落库）。
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
    } else if (currentKind == 3) {
        // gRPC 占位页：支持规划中——只给标签条 + 居中提示，无操作栏/分区。
        rightArea = huxerui::Column {
                        RequestTabStrip(openDrafts, activeTab, envVersion),
                        huxerui::Column {
                            huxerui::Text("gRPC 请求：支持规划中（暂不可保存/发送）",
                                          huxerui::TextRole::Title),
                            huxerui::Text("协议引擎尚未接入，本标签仅作占位，可直接关闭。",
                                          huxerui::TextRole::Body)
                                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
                        }
                            .With(huxerui::Spacing(theme.spacing.small),
                                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                                  huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                                  huxerui::Grow(1.0F))
                            .Key(contentKey),
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
