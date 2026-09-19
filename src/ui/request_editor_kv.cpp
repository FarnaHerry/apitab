// request_editor_kv.cpp — KV 编辑原语（自 request_editor.cpp 拆出，功能域 = KV 表）：
// ReplaceStringList / HeaderValue / 认证头读写 / 类型推断 / CSV 导入导出与
// BatchKvEditor / KvTable 系 composable。跨 TU 供 RequestEditor 调用的函数经
// ui.h 声明（签名仅 draft.h/huxerui/std 类型）；api:: 桥接按 CLAUDE.md 模块
// 约束不进头文件、留在各自调用 TU。
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

void ReplaceStringList(huxerui::StateList<std::string> target,
                       const std::vector<std::string>& values) {
    const std::size_t common = std::min(target.Size(), values.size());
    for (std::size_t i = 0; i < common; ++i) target.Set(i, values[i]);
    while (target.Size() > values.size()) target.PopBack();
    for (std::size_t i = common; i < values.size(); ++i) target.PushBack(values[i]);
}

// kMethodNames / kBodyTypeNames / KvRow / RequestDraft 等共享编辑类型已抽到
// draft.h（测试用例页 / Mock 页同用）；ToKeyValue / FromKeyValue 依赖
// api::KeyValue（模块类型），留在调用方 request_editor.cpp。签名含模块类型，
// 不能进普通头（CLAUDE.md 模块约束）；与 environment_widgets.cpp 的同名桥接
// 同逻辑，P1-C3 归并唯一实现。

std::string HeaderValue(const std::vector<KvRow>& headers, std::string_view name) {
    for (const KvRow& row : headers) {
        std::string key = row.key.text;
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == name && row.enabled) return row.value.text;
    }
    return {};
}

std::size_t AuthModeFromHeaders(const std::vector<KvRow>& headers) {
    const std::string authorization = HeaderValue(headers, "authorization");
    if (authorization.starts_with("Bearer ")) return 1;
    return HeaderValue(headers, "x-api-key").empty() ? 0 : 2;
}

std::string BearerToken(const std::vector<KvRow>& headers) {
    const std::string authorization = HeaderValue(headers, "authorization");
    return authorization.starts_with("Bearer ") ? authorization.substr(7) : std::string{};
}

void SetAuthValue(RequestDraft& draft, std::size_t mode, const std::string& value) {
    std::erase_if(draft.headers, [](const KvRow& row) {
        std::string key = row.key.text;
        std::ranges::transform(key, key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key == "authorization" || key == "x-api-key";
    });
    if (mode == 1 && !value.empty()) {
        draft.headers.push_back(KvRow{.key = huxerui::TextEditingValue{"Authorization"},
                                      .value = huxerui::TextEditingValue{"Bearer " + value}});
    } else if (mode == 2 && !value.empty()) {
        draft.headers.push_back(KvRow{.key = huxerui::TextEditingValue{"X-API-Key"},
                                      .value = huxerui::TextEditingValue{value}});
    }
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

std::string KvRowsToCsv(const std::vector<KvRow>& rows, const KvTableOptions& options) {
    std::string result;
    for (const KvRow& row : rows) {
        if (!result.empty()) result += '\n';
        result += row.enabled ? "true" : "false";
        result += ',';
        result += CsvCell(row.key.text);
        result += ',';
        result += CsvCell(row.value.text);
        if (options.show_type) {
            result += ',';
            result += CsvCell(row.type.text.empty() ? InferKvType(row.value.text) : row.type.text);
        }
        if (options.show_remark) {
            result += ',';
            result += CsvCell(row.remark.text);
        }
    }
    return result;
}

std::optional<std::vector<KvRow>> KvRowsFromCsv(std::string_view csv, std::string& error,
                                                const KvTableOptions& options) {
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
        const std::size_t expectedColumns = 3 + (options.show_type ? 1 : 0) +
                                            (options.show_remark ? 1 : 0);
        if (columns.size() != expectedColumns) {
            error = "第 " + std::to_string(i + 1) + " 行应包含 " +
                    std::to_string(expectedColumns) + " 个字段";
            return std::nullopt;
        }
        std::string enabled = columns[0];
        std::transform(enabled.begin(), enabled.end(), enabled.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (enabled != "true" && enabled != "false" && enabled != "1" && enabled != "0") {
            error = "第 " + std::to_string(i + 1) + " 行的启用字段应为 true/false 或 1/0";
            return std::nullopt;
        }
        std::size_t column = 3;
        const std::string type = options.show_type && !columns[column].empty()
                                     ? columns[column]
                                     : InferKvType(columns[2]);
        if (options.show_type) ++column;
        const std::string remark = options.show_remark ? columns[column] : std::string{};
        rows.push_back(KvRow{.key = huxerui::TextEditingValue{columns[1]},
                             .value = huxerui::TextEditingValue{columns[2]},
                             .type = huxerui::TextEditingValue{type},
                             .remark = huxerui::TextEditingValue{remark},
                             .enabled = enabled == "true" || enabled == "1"});
    }
    return rows;
}

[[huxerui::composable]] huxerui::View BatchKvEditor(
    huxerui::DialogContext ctx, std::vector<KvRow> rows, std::string keyLabel,
    std::string valueLabel, std::function<void(std::vector<KvRow>)> onChanged,
    KvTableOptions options) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto editor = huxerui::codeeditor::UseEditorController();
    auto error = huxerui::UseState(std::string{});
    huxerui::codeeditor::EditorOptions editorOptions;
    ApplyEditorTypography(editorOptions);
    editorOptions.theme = EditorTheme(theme);
    editorOptions.document_key = "batch-kv-csv";
    editorOptions.initial_text = KvRowsToCsv(rows, options);
    editorOptions.wrap_mode = 1;
    editorOptions.sticky_gutter = true;
    return DialogCard(huxerui::Column {
        huxerui::Text("批量编辑", huxerui::TextRole::Title),
        huxerui::Text("每行一条记录；字段顺序：启用, " + keyLabel + ", " + valueLabel +
                          (options.show_type ? ", 类型" : "") +
                          (options.show_remark ? ", 备注" : "") +
                          "。包含逗号或引号的内容请使用 CSV 双引号。",
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
            huxerui::Button("确定").OnClick([ctx, editor, error, onChanged = std::move(onChanged), options] {
                std::string message;
                auto parsed = KvRowsFromCsv(editor.Text(), message, options);
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
    style.outlined.corner_radii = huxerui::CornerRadii{8.0F};
    style.padding = huxerui::EdgeInsets::Symmetric(8.0F, 4.0F);
    return huxerui::ProvideEnvironment(
        style, huxerui::View{huxerui::TextField(std::move(value))
                                 .Label(std::move(label))
                                 .Variant(huxerui::TextFieldVariant::Outlined)
                                 .OnChanged(std::move(onChanged))});
}

// KV 行的类型选择：行内扁平文本触发器（当前值 + 下箭头图标），点击弹自绘下拉选固定类型
// （做法同 MethodUrlBar 的方法触发器；菜单项回调在菜单层关闭后执行，脱离指针
// 事件路径，同步回写即可。选中项用深色填充底色，无对钩）。
[[huxerui::composable]] huxerui::View KvTypeSelect(
    std::string current, std::function<void(std::string)> onChanged) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto popup = huxerui::UsePopup();
    const std::string shown = current.empty() ? "string" : current;
    huxerui::View trigger = huxerui::Row {
        huxerui::Text(shown, huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Image(app::images::chevron_down)
            .Fit(huxerui::ImageFit::Contain)
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
    }
        .With(huxerui::Spacing(2.0F),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
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

std::vector<KvRow> SnapshotKvRows(const huxerui::StateList<KvRow>& rows) {
    return {rows.begin(), rows.end()};
}

// 列表首列勾选框（契约见 ui.h）：命中/指示层尺寸压到列宽常量，空标签时框架把
// 方框水平居中，于是表头（全选框或占位）与数据行共用 kKvCheckColumnWidth。
[[huxerui::composable]] huxerui::View KvColumnCheckbox(
    bool checked, std::function<void(bool)> onChanged) {
    huxerui::CheckboxStyle style = huxerui::UseEnvironment<huxerui::CheckboxStyle>();
    style.minimum_interactive_size = kKvCheckColumnWidth;
    style.state_layer_size = kKvCheckColumnWidth;
    return huxerui::ProvideEnvironment(
        style, huxerui::View{huxerui::Checkbox(checked)
                                 .OnChanged(std::move(onChanged))
                                 .With(huxerui::Frame{.width = kKvCheckColumnWidth})});
}

// 表头全选框（契约见 ui.h）：全选态由调用方按数据行统计给出；点击回调直接透传
// Checkbox 的目标状态（全选 → 取消，否则 → 全选）。部分启用只改无障碍语义，
// 视觉仍是未勾选（Checkbox 没有三态外观）。
// 没有数据行（表里只有虚拟空行）时仍然渲染复选框：表头列几何与有行时一致，
// 用户不会以为"这个表没有全选"；此时禁用（灰）并保持未勾选，没有可全选的对象。
[[huxerui::composable]] huxerui::View KvSelectAllCheckbox(
    std::size_t enabledCount, std::size_t rowCount, std::function<void(bool)> onChanged) {
    const bool empty = rowCount == 0;
    const bool all = !empty && enabledCount == rowCount;
    huxerui::SemanticCheckedState state = huxerui::SemanticCheckedState::Unchecked;
    if (all) {
        state = huxerui::SemanticCheckedState::Checked;
    } else if (!empty && enabledCount > 0) {
        state = huxerui::SemanticCheckedState::Mixed;
    }
    return KvColumnCheckbox(all, [onChanged = std::move(onChanged)](bool checked) {
               if (onChanged) onChanged(checked);
           })
        .With(huxerui::Enabled(!empty),
              huxerui::Tooltip(empty ? "暂无可全选的行" : "全选 / 取消全选"),
              huxerui::Semantics{.label = "全选", .checked = state});
}

// KV 编辑表：StateList 作为虚拟列表的数据源。列表只保留可视区附近的行作用域，
// 末尾额外保留一个虚拟空行；页面若直接持有 StateList，不再在每次重组时复制整表。
[[huxerui::composable]] huxerui::View KvTableStateList(
    huxerui::StateList<KvRow> stateRows, const huxerui::ThemeSpec& theme,
    std::string keyLabel, std::string valueLabel, std::function<void()> onChanged,
    KvTableOptions options) {
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    const auto commitRows = [stateRows, onChanged](std::vector<KvRow> updated) {
        const std::size_t common = std::min(stateRows.Size(), updated.size());
        for (std::size_t i = 0; i < common; ++i) stateRows.Set(i, updated[i]);
        while (stateRows.Size() > updated.size()) stateRows.PopBack();
        for (std::size_t i = common; i < updated.size(); ++i)
            stateRows.PushBack(updated[i]);
        if (onChanged) onChanged();
    };
    // 表头与数据行共用同一套宽度约定：首列勾选框固定 kKvCheckColumnWidth，
    // 键/值/备注自适应拉伸，类型列固定 72pt，尾部动作列固定 88pt。任何一列
    // 只在一侧写死宽度都会让整行与表头错位（首列尤甚），新增列必须两处同源。
    // 首列表头 = 全选框：读一遍数据行统计启用数，点击把全部行设成同一状态
    // （读 StateList 会订阅整表，行内勾选后表头勾选态随之刷新）。
    std::size_t enabledRows = 0;
    for (const KvRow& row : stateRows) {
        if (row.enabled) ++enabledRows;
    }
    const std::size_t rowCount = stateRows.Size();
    const auto typeWidth = huxerui::Frame{.width = 72.0F};
    const auto actionWidth = huxerui::Frame{.width = 88.0F};
    std::vector<huxerui::View> header{
        huxerui::View{KvSelectAllCheckbox(
            enabledRows, rowCount, [stateRows, commitRows](bool enabled) {
                std::vector<KvRow> rows = SnapshotKvRows(stateRows);
                for (KvRow& row : rows) row.enabled = enabled;
                commitRows(std::move(rows));
            })},
        huxerui::Text(keyLabel, huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
        huxerui::Text(valueLabel, huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
    };
    if (options.show_type)
        header.push_back(huxerui::Text("类型", huxerui::TextRole::Label).With(typeWidth));
    if (options.show_remark)
        header.push_back(huxerui::Text("备注", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)));
    header.push_back(
        options.show_batch_edit
            ? huxerui::View{huxerui::Button("批量编辑")
                                 .OnClick([dialog, stateRows, keyLabel, valueLabel, commitRows, options] {
                                     const std::vector<KvRow> rows = SnapshotKvRows(stateRows);
                                     dialog.Show(
                                         [rows, keyLabel, valueLabel, commitRows,
                                          options](huxerui::DialogContext ctx) {
                                             return BatchKvEditor(ctx, rows, keyLabel, valueLabel,
                                                                  commitRows, options);
                                         },
                                         huxerui::DialogOptions{});
                                 })
                                 .With(actionWidth)}
            : huxerui::View{huxerui::Row{}.With(actionWidth)});

    const auto buildRow = [=](std::size_t i) -> huxerui::View {
        const std::size_t rowCount = stateRows.Size();
        const bool phantom = i == rowCount;
        const KvRow row = phantom ? KvRow{} : stateRows.At(i);
        // 行写入：i 越界（虚拟行）时物化新行，否则改写原行。
        // 虚拟行只在真正输入了键/值文本时才物化——聚焦/移动光标触发的
        // OnChanged（text 为空、仅选区变化）不追加新行；只填类型/备注也不物化。
        const auto applyRow = [stateRows, commitRows](std::size_t i, KvRow updated) {
            std::vector<KvRow> copy = SnapshotKvRows(stateRows);
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.key.text.empty() && updated.value.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            commitRows(std::move(copy));
        };
        std::vector<huxerui::View> rowViews{
                KvColumnCheckbox(row.enabled, [row, i, applyRow](bool checked) {
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
        };
        if (options.show_type) {
            rowViews.push_back(KvTypeSelect(row.type.text, [row, i, applyRow](std::string type) {
                KvRow updated = row;
                updated.type = huxerui::TextEditingValue{std::move(type)};
                applyRow(i, std::move(updated));
            }).With(typeWidth));
        }
        if (options.show_remark) {
            rowViews.push_back(
                CompactKvField(row.remark, "备注", [row, i, applyRow](const huxerui::TextEditingValue& value) {
                    KvRow updated = row;
                    updated.remark = value;
                    applyRow(i, std::move(updated));
                }).With(huxerui::Grow(1.0F)));
        }
        rowViews.push_back(phantom
                    ? huxerui::View{huxerui::Row{}.With(actionWidth)}
                    : AppIconButton(app::images::close, "删除此行", [tasks, stateRows, i, commitRows] {
                        // 删除会移除本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            std::vector<KvRow> copy = SnapshotKvRows(stateRows);
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            commitRows(std::move(copy));
                        });
                    }, AppIconButtonShape::Bare).With(actionWidth));
        return huxerui::Column{
                   huxerui::Divider(),
                   huxerui::Row(std::move(rowViews))
                       .With(huxerui::Spacing(theme.spacing.small),
                             huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
               }
            .With(huxerui::Spacing(theme.spacing.extra_small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
            .Key(static_cast<std::uint64_t>(i));
    };

    const std::size_t itemCount = stateRows.Size() + 1; // 末尾虚拟空行
    huxerui::View list = huxerui::VirtualList(itemCount, buildRow)
                             .EstimatedItemExtent(options.show_type || options.show_remark
                                                       ? 52.0F
                                                       : 44.0F)
                             .CacheExtent(160.0F)
                             .With(huxerui::ScrollBar(), huxerui::Grow(1.0F),
                                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    return huxerui::Column{
               huxerui::Row(std::move(header))
                   .With(huxerui::Spacing(theme.spacing.small),
                         huxerui::Foreground(theme.colors.on_surface_variant)),
               std::move(list),
               huxerui::Divider(),
           }
        .With(huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// 请求草稿等领域模型仍由 vector 持有时使用此适配层；控件内部依旧只用官方
// StateList 驱动虚拟列表，并在编辑后通过原有回调回写领域模型。
[[huxerui::composable]] huxerui::View KvTable(
    std::vector<KvRow> rows, const huxerui::ThemeSpec& theme, std::string keyLabel,
    std::string valueLabel, std::function<void(std::vector<KvRow>)> onChanged,
    KvTableOptions options) {
    const auto stateRows = huxerui::UseStateList(std::move(rows));
    return KvTableStateList(
        stateRows, theme, std::move(keyLabel), std::move(valueLabel),
        [stateRows, onChanged = std::move(onChanged)] {
            if (onChanged) onChanged(SnapshotKvRows(stateRows));
        },
        options);
}

} // namespace apitab::ui
