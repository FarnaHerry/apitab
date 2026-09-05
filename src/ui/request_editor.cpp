// request_editor.cpp — 请求编辑器（右上岛）：调试/文档/测试用例/Mock 顶部切换、
// 方法/URL/发送/保存/⋮ 操作栏、Params/Headers/Cookies/Body 分区（KV 表或 Body 编辑器）
// 与保存/发送/Mock 完整路径。自 request_page.cpp 拆出（P1-C1，功能域 = 编辑器），
// 纯搬移；KV 原语（KvTable 等）为单一 owner，供环境表单经 ui.h 声明复用。
#include <huxerui/huxerui.h>
#include <huxerui/codeeditor.h>

#include <algorithm>
#include <array>
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
#include "task_bridge.h"
#include "app_resources.h"

import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.utils;
import nlohmann.json;

namespace apitab::ui {

// kMethodNames / kBodyTypeNames / KvRow / RequestDraft 等共享编辑类型已抽到
// draft.h（测试用例页 / Mock 页同用）；ToKeyValue / FromKeyValue 依赖
// api::KeyValue（模块类型），留在本 TU。签名含模块类型，不能进普通头（CLAUDE.md
// 模块约束）；与 environment_widgets.cpp 的同名桥接同逻辑，P1-C3 归并唯一实现。

inline api::KeyValue ToKeyValue(const KvRow& row) {
    return api::KeyValue{.key = row.key.text,
                         .value = row.value.text,
                         .enabled = row.enabled,
                         .type = row.type.text,
                         .remark = row.remark.text};
}

inline KvRow FromKeyValue(const api::KeyValue& kv) {
    return KvRow{.key = huxerui::TextEditingValue{kv.key},
                 .value = huxerui::TextEditingValue{kv.value},
                 .type = huxerui::TextEditingValue{kv.type},
                 .remark = huxerui::TextEditingValue{kv.remark},
                 .enabled = kv.enabled};
}

// 值 → 类型自动推断（trimming 后判定）：空 → string；true/false → boolean；
// 能完整解析为整数/浮点（可带符号、小数点、科学计数）→ number；其余 → string。
inline std::string InferKvType(const std::string& raw) {
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

std::string CsvCell(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) return std::string{value};
    std::string escaped{"\""};
    for (const char c : value) {
        if (c == '"') escaped += "\"\"";
        else escaped += c;
    }
    escaped += '"';
    return escaped;
}

std::string KvRowsToCsv(const std::vector<KvRow>& rows) {
    std::string result;
    for (const KvRow& row : rows) {
        if (!result.empty()) result += '\n';
        result += row.enabled ? "true" : "false";
        result += ',';
        result += CsvCell(row.key.text);
        result += ',';
        result += CsvCell(row.value.text);
        result += ',';
        result += CsvCell(row.type.text.empty() ? InferKvType(row.value.text) : row.type.text);
        result += ',';
        result += CsvCell(row.remark.text);
    }
    return result;
}

std::optional<std::vector<KvRow>> KvRowsFromCsv(std::string_view csv, std::string& error) {
    std::vector<std::vector<std::string>> records(1);
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < csv.size(); ++i) {
        const char c = csv[i];
        if (quoted) {
            if (c == '"' && i + 1 < csv.size() && csv[i + 1] == '"') {
                field += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field += c;
            }
        } else if (c == '"' && field.empty()) {
            quoted = true;
        } else if (c == ',') {
            records.back().push_back(std::move(field));
            field.clear();
        } else if (c == '\n') {
            records.back().push_back(std::move(field));
            field.clear();
            records.emplace_back();
        } else if (c != '\r') {
            field += c;
        }
    }
    if (quoted) {
        error = "CSV 存在未闭合的引号";
        return std::nullopt;
    }
    records.back().push_back(std::move(field));
    if (records.size() == 1 && records.front().size() == 1 && records.front().front().empty()) return std::vector<KvRow>{};

    std::vector<KvRow> rows;
    rows.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& columns = records[i];
        if (columns.size() != 5) {
            error = "第 " + std::to_string(i + 1) + " 行应包含 5 个字段";
            return std::nullopt;
        }
        std::string enabled = columns[0];
        std::transform(enabled.begin(), enabled.end(), enabled.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (enabled != "true" && enabled != "false" && enabled != "1" && enabled != "0") {
            error = "第 " + std::to_string(i + 1) + " 行的启用字段应为 true/false 或 1/0";
            return std::nullopt;
        }
        rows.push_back(KvRow{
            .key = huxerui::TextEditingValue{columns[1]},
            .value = huxerui::TextEditingValue{columns[2]},
            .type = huxerui::TextEditingValue{columns[3].empty() ? InferKvType(columns[2]) : columns[3]},
            .remark = huxerui::TextEditingValue{columns[4]},
            .enabled = enabled == "true" || enabled == "1",
        });
    }
    return rows;
}

[[huxerui::composable]] huxerui::View BatchKvEditor(
    huxerui::DialogContext ctx, std::vector<KvRow> rows, std::string keyLabel,
    std::string valueLabel, std::function<void(std::vector<KvRow>)> onChanged) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto editor = huxerui::codeeditor::UseEditorController();
    auto error = huxerui::UseState(std::string{});
    huxerui::codeeditor::EditorOptions editorOptions;
    ApplyEditorTypography(editorOptions);
    editorOptions.theme = EditorTheme(theme);
    editorOptions.document_key = "batch-kv-csv";
    editorOptions.initial_text = KvRowsToCsv(rows);
    editorOptions.wrap_mode = 1;
    editorOptions.sticky_gutter = true;
    return DialogCard(huxerui::Column {
        huxerui::Text("批量编辑", huxerui::TextRole::Title),
        huxerui::Text("每行一条记录；字段顺序：启用, " + keyLabel + ", " + valueLabel +
                          ", 类型, 备注。包含逗号或引号的内容请使用 CSV 双引号。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Text("CSV 表单数据", huxerui::TextRole::Label),
        huxerui::codeeditor::CodeEditor(std::move(editorOptions), editor)
            .With(huxerui::Frame{.height = 280.0F}),
        error.Get().empty()
            ? huxerui::View{huxerui::Row{}}
            : huxerui::View{huxerui::Text(error.Get(), huxerui::TextRole::Body)
                                 .With(huxerui::Foreground(theme.colors.error))},
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("确定").OnClick([ctx, editor, error, onChanged = std::move(onChanged)] {
                std::string message;
                auto parsed = KvRowsFromCsv(editor.Text(), message);
                if (!parsed.has_value()) {
                    error = std::move(message);
                    return;
                }
                onChanged(std::move(*parsed));
                ctx.Dismiss();
            }),
        }.With(huxerui::Spacing(8.0F),
               huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }.With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 620.0F},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View CompactKvField(
    huxerui::TextEditingValue value, std::string label,
    std::function<void(const huxerui::TextEditingValue&)> onChanged) {
    huxerui::TextFieldStyle style = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    style.show_label = false;
    style.outlined.border = huxerui::Color::Transparent();
    style.outlined.minimum_height = 30.0F;
    style.corner_radius = 8.0F;
    style.padding = huxerui::EdgeInsets::Symmetric(8.0F, 4.0F);
    return huxerui::ProvideEnvironment(
        style, huxerui::View{huxerui::TextField(std::move(value))
                                 .Label(std::move(label))
                                 .Variant(huxerui::TextFieldVariant::Outlined)
                                 .OnChanged(std::move(onChanged))});
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
    auto dialog = huxerui::UseDialog();
    // 表头与数据行共用同一套宽度约定：勾选框约 24pt，键/值/备注自适应拉伸，
    // 类型列固定 72pt。
    const auto typeWidth = huxerui::Frame{.width = 72.0F};
    const auto actionWidth = huxerui::Frame{.width = 88.0F};
    std::vector<huxerui::View> children{
        huxerui::Row {
            huxerui::Text("", huxerui::TextRole::Label)
                .With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text(keyLabel, huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
            huxerui::Text(valueLabel, huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
            huxerui::Text("类型", huxerui::TextRole::Label).With(typeWidth),
            huxerui::Text("备注", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
            huxerui::Button("批量编辑")
                .OnClick([dialog, rows, keyLabel, valueLabel, onChanged] {
                    dialog.Show(
                        [rows, keyLabel, valueLabel, onChanged](huxerui::DialogContext ctx) {
                            return BatchKvEditor(ctx, rows, keyLabel, valueLabel, onChanged);
                        },
                        huxerui::DialogOptions{});
                })
                .With(actionWidth),
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
        children.push_back(huxerui::Divider());
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(row.enabled).OnChanged([row, i, applyRow](bool checked) {
                    KvRow updated = row;
                    updated.enabled = checked;
                    applyRow(i, std::move(updated));
                }),
                CompactKvField(row.key, "键", [row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.key = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                CompactKvField(row.value, "值", [row, i, applyRow](const huxerui::TextEditingValue& value) {
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
                CompactKvField(row.remark, "备注", [row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.remark = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                phantom
                    ? huxerui::View{huxerui::Row{}.With(
                          huxerui::Frame{.width = 88.0F, .height = 28.0F})}
                    : AppIconButton("✕", "删除此行", [tasks, rows, i, onChanged] {
                        // 删除会移除本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<KvRow> copy = rows;
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            onChanged(std::move(copy));
                        });
                    }, AppIconButtonShape::Bare).With(actionWidth),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    children.push_back(huxerui::Divider());

    return huxerui::Column(std::move(children))
        .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 测试用例 / Mock：草稿编辑形态 ⇄ db 落库形态 ----
// 数字字段草稿侧用文本承载（受控 TextField），空/非法按"不校验"或默认值换算。

inline int ParseIntField(const huxerui::TextEditingValue& v, int fallback = 0) {
    const std::string s = trim(v.text);
    int out = fallback;
    if (!s.empty()) {
        std::from_chars(s.data(), s.data() + s.size(), out);
    }
    return out;
}

inline db::RequestTestCase CaseToDb(const TestCaseDraft& c) {
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


inline db::RequestMock MockToDb(const MockDraft& m) {
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
inline api::RequestSpec SpecFromDraft(const RequestDraft& draft) {
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

// 响应头是否声明 SSE（引擎保证进度/结果快照里的头部来自最后一个头部块，
// 重定向中间块不参与判断）。
inline bool IsEventStreamResponse(const std::vector<api::KeyValue>& headers) {
    for (const api::KeyValue& h : headers) {
        std::string key = h.key;
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        if (key != "content-type") continue;
        std::string value = h.value;
        std::ranges::transform(value, value.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        if (value.find("text/event-stream") != std::string::npos) return true;
    }
    return false;
}

// 从响应头里提取 Set-Cookie 条目（键大小写不敏感），一行一个：
// "name = value; 属性..."（首个 '=' 换成 ' = ' 便于阅读，属性原样保留）。
inline std::vector<std::string> CookiesFromHeaders(const std::vector<api::KeyValue>& headers) {
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
    // 是否已收到在途传输的增量进度（流式/分块正文）；取消时据此保留已接收内容。
    auto streamed = huxerui::UseState(false);
    // Body 编辑器控制器：格式化按钮与 BodyTextEditor 共享（SweetEditor 非受控，
    // 外部改文本须 LoadDocument）。在顶层创建保证切分区时 UseState 槽位稳定。
    auto bodyEditorController = huxerui::codeeditor::UseEditorController();
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
                        [drafts, index, tasks, bodyEditorController] {
                        const std::vector<RequestDraft> current = drafts.Get();
                        if (index >= current.size()) return;
                        const std::size_t kind = current[index].bodyKindIndex;
                        if (kind != 1 && kind != 5) return; // 仅 JSON / XML 可格式化
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
                                    pretty);
                                MutateDraft(drafts, index, [&](RequestDraft& d) {
                                    d.bodies[kind] = huxerui::TextEditingValue{std::move(pretty)};
                                });
                            } catch (...) {
                                // 格式不合法时保留原文并静默结束，不用瞬时提示打断编辑。
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
                        if (streamed.Get()) {
                            // 流式响应：已接收内容保留，只把状态行标记为取消。
                            // 进度文本格式固定为 "状态行\n\n正文"（本文件构建）。
                            const std::string text = responseBody.Get();
                            const auto sep = text.find("\n\n");
                            responseBody = "已取消（流已断开）" +
                                (sep == std::string::npos ? std::string{}
                                                          : text.substr(sep));
                            streamed = false;
                        } else {
                            responseBody = "已取消";
                        }
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
                        // 每拍先 drain 增量进度：SSE（逐条/逐字到达）与分块正文
                        // 实时呈现，最终结果仍由 takeResponse 投递。
                        api::ResponseView view;
                        co_await PollWhile(std::chrono::duration<double>{0.03}, [&] {
                            api::ResponseView progress;
                            while (g_requests.takeProgress(progress)) {
                                streamed = true;
                                const std::string statusLine =
                                    (IsEventStreamResponse(progress.headers) ? "SSE · " : "") +
                                    std::format("HTTP {} · 接收中 · {} bytes", progress.status,
                                                progress.body.size());
                                responseBody = statusLine + "\n\n" + progress.body;
                                std::vector<std::string> lines;
                                for (const api::KeyValue& h : progress.headers)
                                    lines.push_back(h.key + ": " + h.value);
                                responseHeaders = lines;
                                responseCookies = CookiesFromHeaders(progress.headers);
                            }
                            return !g_requests.takeResponse(view);
                        });
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
                        } else if (streamed.Get() && !view.body.empty()) {
                            // 流式传输中断（连接重置等）：保留已接收内容。
                            responseBody = "连接中断: " + view.error + "\n\n" + view.body;
                            std::vector<std::string> lines;
                            for (const api::KeyValue& h : view.headers)
                                lines.push_back(h.key + ": " + h.value);
                            responseHeaders = lines;
                            responseCookies = CookiesFromHeaders(view.headers);
                        } else {
                            responseBody = "请求失败: " + view.error;
                        }
                        streamed = false;
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
// 悬停菜单必须关 outside-press：Barrier 层盖满视口会截断悬停路由，Show 后箭头立刻
// 收到 Leave、Dismiss 后又收到 Enter，形成展开/关闭闪烁循环。改用 Content 层 +
// 箭头/菜单双向悬停跟踪，双方都离开后经 150ms 宽限再关（指针跨过 4dp 间隙进菜单
// 不会误关）；Esc/点条目仍经原 dismiss 链关闭。
[[huxerui::composable]] huxerui::View SplitActionButton(
    std::string label, std::function<void(bool)> onAction, std::string alternateLabel,
    bool alternateEnabled) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto popup = huxerui::UsePopup();
    auto tasks = huxerui::UseTaskScope();
    struct HoverMenuState {
        huxerui::LayerId layer = 0;
        bool arrowHovered = false;
        bool menuHovered = false;
        std::uint64_t generation = 0; // 每次 Enter 递增，作废未生效的宽限关闭任务
    };
    // 非响应式状态只记录当前层与悬停来源；Enter 只 Show 一次，关闭走宽限任务，
    // 按钮不重组。
    auto hover = huxerui::UseState(std::make_shared<HoverMenuState>()).Get();
    auto closeIfLeft = [popup, hover, tasks] {
        if (hover->layer == 0) return;
        const std::uint64_t generation = hover->generation;
        tasks.Launch([popup, hover, generation]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0.15});
            if (generation != hover->generation || hover->arrowHovered || hover->menuHovered ||
                hover->layer == 0)
                co_return;
            popup.Dismiss(hover->layer);
            hover->layer = 0;
        });
    };
    auto showAlternates = [popup, alternateLabel, onAction, alternateEnabled, hover, closeIfLeft] {
        if (!alternateEnabled) return;
        if (hover->layer != 0) return; // 同一次 hover 只挂一层，避免重复 Show 闪烁
        hover->layer = ShowHoverAppMenu(
            popup,
            {AppMenuItem{.label = alternateLabel,
                         .onClick = [onAction, hover] {
                             hover->layer = 0;
                             onAction(true);
                         }}},
            [hover, closeIfLeft](bool menuHovered) {
                if (menuHovered) {
                    ++hover->generation;
                    hover->menuHovered = true;
                } else {
                    hover->menuHovered = false;
                    closeIfLeft();
                }
            },
            huxerui::PopupOptions{
                .placement = {huxerui::AnchorSide::Below,
                              huxerui::AnchorAlignment::End},
                .dismiss_on_outside_press = false,
                .on_dismiss_request = [hover] { hover->layer = 0; }});
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
                [hover, showAlternates, closeIfLeft](const huxerui::HoverEvent& event) {
                    if (event.type == huxerui::HoverEventType::Enter) {
                        ++hover->generation;
                        hover->arrowHovered = true;
                        showAlternates();
                    } else if (event.type == huxerui::HoverEventType::Leave) {
                        hover->arrowHovered = false;
                        closeIfLeft();
                    }
                }),
    }.With(huxerui::Background(theme.colors.primary),
           huxerui::CornerRadius(islands.control_radius), huxerui::ClipChildren(),
           // 分裂按钮必须是固定自然宽度；若不钳制，外层 Row 会把它当可扩张
           // 容器吞掉操作栏余量，后面的“保存”被挤出屏幕。URL 栏才是唯一 Grow 项。
           huxerui::Frame{.width = 84.0F, .height = islands.control_height},
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}

} // namespace apitab::ui
