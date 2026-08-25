// ui/widgets.cppm — 可复用控件：rail 导航项 / 小节标签 / 方法徽章色 /
// KV 编辑器（Params 与 Headers 共用）/ 底部状态条。
module;

#include "eui_ui.h"

export module apitab.ui.widgets;

import std;
import apitab.api_engine;
import apitab.ui.theme;
import apitab.ui.utils;

export void drawIslandPanel(eui::Ui& ui, const std::string& id, float x, float y,
                            float w, float h, const AppTheme& theme,
                            float opacity = 0.92f) {
    if (w <= 0.0f || h <= 0.0f) return;
    components::panel(ui, id, theme.components)
        .position(x, y)
        .size(w, h)
        .opacity(std::clamp(opacity, 0.0f, 1.0f))
        .build();
}



export void drawRailItem(eui::Ui& ui, const std::string& id, float y, float railWidth,
                         unsigned int icon, bool active, const AppTheme& theme,
                         std::function<void()> onClick) {
    const auto& tokens = theme.components;
    const auto transition = core::Transition::make(0.14f, core::Ease::OutCubic);
    const core::Color idle = {0.0f, 0.0f, 0.0f, 0.0f};
    const core::Color activeFill =
        components::theme::withAlpha(tokens.primary, theme.dark ? 0.22f : 0.14f);
    const core::Color iconColor = active ? tokens.primary : tokens.text;
    const float itemW = railWidth - 8.0f;
    const float x = (railWidth - itemW) * 0.5f;

    ui.rect(id + ".bg")
        .position(x, y)
        .size(itemW, 24.0f)
        .color(active ? activeFill : idle)
        .radius(7.0f)
        .transition(transition)
        .build();

    if (active) {
        ui.rect(id + ".bar")
            .position(0, y + 4.0f)
            .size(2.0f, 16.0f)
            .color(tokens.primary)
            .radius(1.0f)
            .build();
    }

    ui.rect(id + ".hit")
        .position(x, y)
        .size(itemW, 24.0f)
        .states(idle, active ? idle : tokens.surfaceHover, tokens.surfaceActive)
        .radius(7.0f)
        .transition(transition)
        .onClick(std::move(onClick))
        .build();

    ui.text(id + ".icon")
        .position(0, y)
        .size(railWidth, 24.0f)
        .icon(icon)
        .fontSize(13.0f)
        .lineHeight(24.0f)
        .color(iconColor)
        .horizontalAlign(core::HorizontalAlign::Center)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}

// ---- 主色上的文字/图标色（对齐 tinynext onPrimaryColor）----
// 主色是白（深主题）/黑（浅主题），primary 按钮与 segmented 选中块的文字必须
// 反色才可读 —— eui 组件默认按「主色是深色」假设配近白字，这里按主题翻转。

export core::Color onPrimaryColor(const AppTheme& theme) {
    return theme.components.dark ? core::Color{0.03f, 0.03f, 0.03f, 1.0f}
                                 : core::Color{0.95f, 0.95f, 0.95f, 1.0f};
}

// segmented 选中文字色修正（默认 selectedText 恒近白，白底主色下不可读）。
export components::SegmentedStyle segmentedStyle(const AppTheme& theme) {
    components::SegmentedStyle style(theme.components);
    style.selectedText = onPrimaryColor(theme);
    return style;
}

// EUI 的 dropdown 固定向下展开；底部分页等场景改用此控件。
export void drawListPicker(eui::Ui& ui, const std::string& id, float width, float height,
                           const AppTheme& theme, bool& open,
                           const std::vector<std::string>& items, int selected, bool opensUp,
                           std::function<void(int)> onPick) {
    constexpr float itemHeight = 22.0f;
    constexpr float popupPad = 3.0f;
    constexpr float popupGap = 3.0f;
    const float popupHeight = itemHeight * static_cast<float>(items.size()) + popupPad * 2.0f;
    const auto& tokens = theme.components;
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(items.size()) - 1));

    ui.stack(id)
        .size(width, height)
        .zIndex(30)
        .content([&] {
            ui.rect(id + ".field")
                .size(width, height)
                .states(tokens.surface, tokens.surfaceHover, tokens.surfaceActive)
                .radius(6.0f)
                .border(1.0f, components::theme::withAlpha(tokens.border, 0.78f))
                .onClick([&open] { open = !open; })
                .build();
            ui.text(id + ".label")
                .position(8.0f, 0)
                .size(width - 28.0f, height)
                .text(items.empty() ? "" : items[selected])
                .fontSize(kFontLabel)
                .lineHeight(height)
                .color(tokens.text)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            ui.text(id + ".chevron")
                .position(width - 20.0f, 0)
                .size(14.0f, height)
                .icon(open ? 0xF077 : 0xF078)
                .fontSize(9.0f)
                .lineHeight(height)
                .color(tokens.primary)
                .horizontalAlign(core::HorizontalAlign::Center)
                .verticalAlign(core::VerticalAlign::Center)
                .build();

            if (!open || items.empty()) return;
            ui.rect(id + ".dismiss")
                .position(-2000.0f, -2000.0f)
                .size(5000.0f, 5000.0f)
                .color({0.0f, 0.0f, 0.0f, 0.0f})
                .onClick([&open] { open = false; })
                .onScroll([](const core::ScrollEvent&) {})
                .build();
            ui.stack(id + ".popup")
                .position(0, opensUp ? -(popupHeight + popupGap) : height + popupGap)
                .size(width, popupHeight)
                .zIndex(31)
                .content([&] {
                    ui.rect(id + ".popup.bg")
                        .size(width, popupHeight)
                        .color(tokens.surface)
                        .radius(6.0f)
                        .border(1.0f, components::theme::withAlpha(tokens.border, 0.78f))
                        .onClick([] {})
                        .build();
                    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                        const float itemY = popupPad + static_cast<float>(i) * itemHeight;
                        ui.rect(id + ".item." + std::to_string(i))
                            .position(popupPad, itemY)
                            .size(width - popupPad * 2.0f, itemHeight)
                            .states(i == selected
                                        ? components::theme::withAlpha(tokens.primary, 0.16f)
                                        : core::Color{0, 0, 0, 0},
                                    tokens.surfaceHover, tokens.surfaceActive)
                            .radius(5.0f)
                            .onClick([&open, i, onPick] {
                                open = false;
                                onPick(i);
                            })
                            .build();
                        ui.text(id + ".item.label." + std::to_string(i))
                            .position(popupPad + 8.0f, itemY)
                            .size(width - popupPad * 2.0f - 16.0f, itemHeight)
                            .text(items[i])
                            .fontSize(kFontLabel)
                            .lineHeight(itemHeight)
                            .color(i == selected ? tokens.primary : tokens.text)
                            .verticalAlign(core::VerticalAlign::Center)
                            .build();
                    }
                })
                .build();
        })
        .build();
}

// ---- 小节标签 ----
export void drawSectionLabel(eui::Ui& ui, const std::string& id, float x, float y,
                             float w, const std::string& text, const AppTheme& theme) {
    ui.text(id)
        .position(x, y)
        .size(w, 16.0f)
        .text(text)
        .fontSize(kFontLabel)
        .color(theme.metaText)
        .build();
}

// ---- HTTP 方法徽章色（侧栏 / 历史列表共用）----

export eui::Color methodColor(const std::string& method, const AppTheme& theme) {
    if (method == "GET") return theme.ok;
    if (method == "POST") return theme.redirect;
    if (method == "PUT" || method == "PATCH") return theme.clientErr;
    if (method == "DELETE") return theme.serverErr;
    return theme.idle;
}

// ---- KV 编辑器（Params / Headers 共用）----

namespace {

std::unordered_map<std::string, bool> g_paramTypeOpen;

} // namespace
// 每行 [key input][value input][× 删除]，底部「+ 添加」按钮。
// 行 id 用下标（KV 场景可接受：删除中间行后焦点跳变，无状态错乱）。
//
// 布局纪律：本函数组合在 scrollView 的 content 里 —— content 是 column 弹性
// 容器，直接 .position() 的子项会被重排（每个元素占一个竖排槽位）。所以每行
// 必须包一层 stack 占位，行内元素在 stack 里绝对定位（eui 官方示例模式）。

export float drawKvEditor(eui::Ui& ui, const std::string& id, float x, float y, float w,
                          std::vector<api::KeyValue>& items, const AppTheme& theme) {
    const float delW = 22.0f;
    const float colGap = 4.0f;
    const float keyW = (w - delW - colGap * 2.0f) * 0.42f;
    const float valW = w - delW - colGap * 2.0f - keyW;

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const std::string rowId = id + "." + std::to_string(i);
        ui.stack(rowId)
            .position(x, y)  // scroll content 里由 column 分配槽位；x/y 仅直接组合时生效
            .size(w, kInputHeight)
            .content([&] {
                components::input(ui, rowId + ".k")
                    .position(0, 0)
                    .size(keyW, kInputHeight)
                    .value(items[i].key)
                    .placeholder("Key")
                    .theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].key = v; })
                    .build();
                components::input(ui, rowId + ".v")
                    .position(keyW + colGap, 0)
                    .size(valW, kInputHeight)
                    .value(items[i].value)
                    .placeholder("Value")
                    .theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].value = v; })
                    .build();
                components::button(ui, rowId + ".del")
                    .position(keyW + colGap + valW + colGap, 2.0f)
                    .size(delW, delW)
                    .icon(0xF00D)  // fa-xmark
                    .text("")
                    .iconSize(9.0f)
                    .theme(theme.components, false)
                    .onClick([&items, i] { items.erase(items.begin() + i); })
                    .build();
            })
            .build();
    }

    components::button(ui, id + ".add")
        .position(x, y)  // 同上：直接组合时生效，scroll content 里由 column 布局
        .size(64.0f, 22.0f)
        .icon(0xF067)  // fa-plus
        .text("添加")
        .fontSize(kFontLabel)
        .theme(theme.components, false)
        .onClick([&items] { items.push_back({}); })
        .build();
    return y + items.size() * (kRowHeight + colGap) + 22.0f;
}

export float drawParamEditor(eui::Ui& ui, const std::string& id, float x, float y, float w,
                             std::vector<api::KeyValue>& items, const AppTheme& theme) {
    const float delW = 22.0f;
    const float gap = 4.0f;
    const float rowH = kInputHeight * 2.0f + gap;
    const float keyW = std::max(96.0f, (w - delW - gap * 4.0f) * 0.25f);
    const float valueW = std::max(96.0f, (w - delW - gap * 4.0f) * 0.25f);
    const float typeW = 92.0f;
    const float remarkW = std::max(80.0f, w - keyW - valueW - typeW - delW - gap * 4.0f);
    const std::vector<std::string> types = {"string", "integer", "number", "boolean"};

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const std::string rowId = id + ".param." + std::to_string(i);
        ui.stack(rowId)
            .position(x, y)
            .size(w, rowH)
            .content([&] {
                components::input(ui, rowId + ".k")
                    .position(0, 0).size(keyW, kInputHeight)
                    .value(items[i].key).placeholder("Key").theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].key = v; }).build();
                components::input(ui, rowId + ".v")
                    .position(keyW + gap, 0).size(valueW, kInputHeight)
                    .value(items[i].value).placeholder("Value").theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].value = v; }).build();
                ui.stack(rowId + ".type.wrap")
                    .position(keyW + valueW + gap * 2.0f, 0)
                    .size(typeW, kInputHeight)
                    .content([&] {
                        components::dropdown(ui, rowId + ".type")
                            .size(typeW, kInputHeight)
                            .items(types)
                            .selected([&] {
                                const auto it = std::ranges::find(types, items[i].type);
                                return it == types.end() ? 0 : static_cast<int>(it - types.begin());
                            }())
                            .open(g_paramTypeOpen[rowId])
                            .theme(theme.components)
                            .onOpenChange([key = rowId](bool open) { g_paramTypeOpen[key] = open; })
                            .onChange([&items, i, types, key = rowId](int selected) {
                                items[i].type = types[std::clamp(selected, 0, static_cast<int>(types.size()) - 1)];
                                g_paramTypeOpen[key] = false;
                            })
                            .build();
                    })
                    .build();
                components::input(ui, rowId + ".remark")
                    .position(keyW + valueW + typeW + gap * 3.0f, 0)
                    .size(remarkW, kInputHeight)
                    .value(items[i].remark).placeholder("备注").theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].remark = v; }).build();
                components::button(ui, rowId + ".del")
                    .position(w - delW, 2.0f).size(delW, delW)
                    .icon(0xF00D).text("").iconSize(9.0f)
                    .theme(theme.components, false)
                    .onClick([&items, i] { items.erase(items.begin() + i); }).build();
            })
            .build();
    }
    components::button(ui, id + ".param.add")
        .position(x, y).size(64.0f, 22.0f)
        .icon(0xF067).text("添加").fontSize(kFontLabel)
        .theme(theme.components, false)
        .onClick([&items] { items.push_back({.type = "string"}); }).build();
    return y + items.size() * (rowH + kGap) + 22.0f;
}

export void drawStatusBar(eui::Ui& ui, float width, float height,
                          const std::string& message, const AppTheme& theme) {
    (void)ui;
    (void)width;
    (void)height;
    (void)message;
    (void)theme;
}
