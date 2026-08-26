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
    const auto& tokens = theme.components;
    const core::Color base = core::mixColor(tokens.background, tokens.surface, 0.5f);
    const core::Color fill{base.r, base.g, base.b,
                           std::clamp(opacity, 0.0f, 1.0f)};
    const core::Color shadow = theme.dark
        ? core::Color{0.0f, 0.0f, 0.0f, 0.25f}
        : core::Color{0.10f, 0.14f, 0.22f, 0.12f};
    ui.rect(id)
        .position(x, y)
        .size(w, h)
        .color(fill)
        .radius(kIslandRadius)
        .border(1.0f, components::theme::withOpacity(tokens.border, 0.60f))
        .shadow(14.0f, 3.0f, shadow)
        .build();
}

export void drawIslandDivider(eui::Ui& ui, const std::string& id, float x, float y,
                              float h, const AppTheme& theme) {
    constexpr float inset = 2.0f;
    ui.rect(id)
        .position(x, y + inset)
        .size(1.0f, std::max(0.0f, h - inset * 2.0f))
        .color(components::theme::withOpacity(theme.components.border, 0.55f))
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

export components::CheckboxStyle checkboxStyle(const AppTheme& theme) {
    components::CheckboxStyle style(theme.components);
    // apitab 深色主题的 checked 背景是白色 primary；EUI 默认 mark 也是白色，
    // 覆写为 primary 对比色后勾选标记才可见。
    style.mark = onPrimaryColor(theme);
    return style;
}
// segmented 选中文字色修正（默认 selectedText 恒近白，白底主色下不可读）。
export components::SegmentedStyle segmentedStyle(const AppTheme& theme) {
    components::SegmentedStyle style(theme.components);
    style.selectedText = onPrimaryColor(theme);
    return style;
}

namespace {

struct SelectionPopupEntry {
    std::string id;
    bool open = false;
    bool seen = false;
    std::function<void()> close;
};

std::vector<SelectionPopupEntry> g_selectionPopups;

SelectionPopupEntry* findSelectionPopup(const std::string& id) {
    for (auto& entry : g_selectionPopups) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

} // namespace

// Native dropdown 不带全屏 outside-dismiss；页面在每帧注册当前可见的选择浮层，
// app 根组合在所有普通内容之后绘制统一的透明 blocker。状态仍归页面所有，
// registry 只负责互斥、关闭回调和 blocker 的命中层级。
export void beginSelectionPopupFrame() {
    for (auto& entry : g_selectionPopups) entry.seen = false;
}

export void registerSelectionPopup(const std::string& id, bool open,
                                   std::function<void()> close) {
    if (SelectionPopupEntry* entry = findSelectionPopup(id)) {
        entry->open = open;
        entry->seen = true;
        entry->close = std::move(close);
        return;
    }
    g_selectionPopups.push_back({id, open, true, std::move(close)});
}

export void setSelectionPopupOpen(const std::string& id, bool open) {
    SelectionPopupEntry* entry = findSelectionPopup(id);
    if (entry == nullptr) return;
    if (open) {
        for (auto& entry : g_selectionPopups) {
            if (entry.id == id) continue;
            if (entry.seen && entry.open) {
                entry.open = false;
                if (entry.close) entry.close();
            }
        }
    }
    entry->open = open;
}

export void closeSelectionPopups() {
    for (auto& entry : g_selectionPopups) {
        if (entry.seen && entry.open && entry.close) entry.close();
        entry.open = false;
    }
}

export void drawSelectionPopupDismissLayer(eui::Ui& ui, const eui::Screen& screen) {
    for (auto it = g_selectionPopups.begin(); it != g_selectionPopups.end();) {
        if (!it->seen) {
            if (it->open && it->close) it->close();
            it = g_selectionPopups.erase(it);
        } else {
            ++it;
        }
    }
    bool anyOpen = false;
    for (const auto& entry : g_selectionPopups) anyOpen = anyOpen || entry.open;
    if (!anyOpen || screen.width <= 0.0f || screen.height <= 0.0f) return;
    ui.rect("selection.popups.dismiss")
        .size(screen.width, screen.height)
        .states({0.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f},
                {0.0f, 0.0f, 0.0f, 0.0f})
        .zIndex(10)
        .onClick([] { closeSelectionPopups(); })
        .onScroll([](const core::ScrollEvent&) {})
        .build();
}

// EUI 的 dropdown 固定向下展开；底部分页等场景改用此控件。
// anchorScreenY/screenHeight 提供控件在屏幕坐标系中的位置后，popup 高度会被
// clamp 到屏幕内，并在上下两个方向中选空间更大的一侧展开；不传（默认 <0）
// 保持旧的纯 opensUp 行为。
export void drawListPicker(eui::Ui& ui, const std::string& id, float width, float height,
                           const AppTheme& theme, bool& open,
                           const std::vector<std::string>& items, int selected, bool opensUp,
                           std::function<void(int)> onPick,
                           float anchorScreenY = -1.0f, float screenHeight = 0.0f) {
    constexpr float itemHeight = 22.0f;
    constexpr float popupPad = 3.0f;
    constexpr float popupGap = 3.0f;
    float popupHeight = itemHeight * static_cast<float>(items.size()) + popupPad * 2.0f;
    bool up = opensUp;
    if (anchorScreenY >= 0.0f && screenHeight > 0.0f) {
        const float below = nonNegative(screenHeight - (anchorScreenY + height + popupGap));
        const float above = nonNegative(anchorScreenY - popupGap);
        if (up && popupHeight > above && below > above) up = false;
        if (!up && popupHeight > below && above > below) up = true;
        popupHeight = std::min(popupHeight, up ? above : below);
    }
    const auto& tokens = theme.components;
    selected = std::clamp(selected, 0, std::max(0, static_cast<int>(items.size()) - 1));
    registerSelectionPopup(id, open, [&open] { open = false; });

    ui.stack(id)
        .size(width, height)
        .zIndex(30)
        .content([&] {
            ui.rect(id + ".field")
                .size(width, height)
                .states(tokens.surface, tokens.surfaceHover, tokens.surfaceActive)
                .radius(6.0f)
                .border(1.0f, components::theme::withAlpha(tokens.border, 0.78f))
                .onClick([&open, id] {
                    open = !open;
                    setSelectionPopupOpen(id, open);
                })
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

            if (!open || items.empty() || popupHeight <= 0.0f) return;
            ui.stack(id + ".popup")
                .position(0, up ? -(popupHeight + popupGap) : height + popupGap)
                .size(width, popupHeight)
                .zIndex(31)
                .clip()
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
                            .onClick([&open, id, i, onPick] {
                                open = false;
                                setSelectionPopupOpen(id, false);
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

export void drawEditorDivider(eui::Ui& ui, const std::string& id, float x, float y,
                              float width, const AppTheme& theme) {
    ui.rect(id)
        .position(x, y)
        .size(nonNegative(width), kEditorDividerHeight)
        .color(components::theme::withOpacity(theme.components.border, 0.45f))
        .build();
}


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
    const float delW = kIconButtonSize;
    const float colGap = kEditorRowGap;
    // 行内可用宽先 clamp 非负：w 小于删除按钮+间距时 key/value 收缩到 0 而不是
    // 变负，删除按钮也留在行内右端。
    const float innerW = nonNegative(w - delW - colGap * 2.0f);
    const float keyW = innerW * 0.42f;
    const float valW = innerW - keyW;
    const float delX = std::min(innerW + colGap, nonNegative(w - delW));

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const std::string rowId = id + "." + std::to_string(i);
        ui.stack(rowId)
            .position(x, y)  // scroll content 里由 column 分配槽位；x/y 仅直接组合时生效
            .size(w, kEditorRowHeight)
            .content([&] {
                components::input(ui, rowId + ".k")
                    .position(0, 0)
                    .size(keyW, kEditorRowHeight)
                    .value(items[i].key)
                    .placeholder("Key")
                    .theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].key = v; })
                    .build();
                components::input(ui, rowId + ".v")
                    .position(keyW + colGap, 0)
                    .size(valW, kEditorRowHeight)
                    .value(items[i].value)
                    .placeholder("Value")
                    .theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].value = v; })
                    .build();
                components::button(ui, rowId + ".del")
                    .position(delX, 2.0f)
                    .size(delW, delW)
                    .icon(0xF00D)  // fa-xmark
                    .text("")
                    .iconSize(9.0f)
                    .theme(theme.components, false)
                    .radius(kIconButtonRadius)
                    .onClick([&items, i] { items.erase(items.begin() + i); })
                    .build();
                drawEditorDivider(ui, rowId + ".divider", 0,
                                  kEditorRowHeight - kEditorDividerHeight, w, theme);
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
        .radius(kButtonRadius)
        .onClick([&items] { items.push_back({}); })
        .build();
    return y + items.size() * (kEditorRowHeight + colGap) + 22.0f;
}

export float drawFieldTable(eui::Ui& ui, const std::string& id, float x, float y, float w,
                            std::vector<api::KeyValue>& items, const AppTheme& theme,
                            bool showMeta = true) {
    const auto& tokens = theme.components;
    constexpr float headerH = 24.0f;
    constexpr float rowH = 28.0f;
    const float gap = kEditorRowGap;
    const float actionW = kIconButtonSize;
    const float enabledW = 24.0f;
    const float usable = nonNegative(w - enabledW - actionW - gap * 4.0f);
    const float keyW = usable * (showMeta ? 0.24f : 0.38f);
    const float valueW = usable * (showMeta ? 0.27f : 0.52f);
    const float typeW = showMeta ? 86.0f : 0.0f;
    const float remarkW = showMeta ? nonNegative(usable - keyW - valueW - typeW) : 0.0f;
    const auto colX = [&](float offset) { return x + offset; };

    ui.stack(id + ".header").position(x, y).size(w, headerH).content([&] {
        ui.text(id + ".header.enabled").position(0, 0).size(enabledW, headerH)
            .text("启").fontSize(kFontLabel).lineHeight(headerH).color(theme.metaText)
            .horizontalAlign(core::HorizontalAlign::Center).verticalAlign(core::VerticalAlign::Center).build();
        ui.text(id + ".header.key").position(colX(enabledW + gap), 0).size(keyW, headerH)
            .text("Key").fontSize(kFontLabel).lineHeight(headerH).color(theme.metaText).build();
        ui.text(id + ".header.value").position(colX(enabledW + gap * 2 + keyW), 0).size(valueW, headerH)
            .text("Value").fontSize(kFontLabel).lineHeight(headerH).color(theme.metaText).build();
        if (showMeta) {
            ui.text(id + ".header.type").position(colX(enabledW + gap * 3 + keyW + valueW), 0).size(typeW, headerH)
                .text("类型").fontSize(kFontLabel).lineHeight(headerH).color(theme.metaText).build();
            ui.text(id + ".header.remark").position(colX(enabledW + gap * 3 + keyW + valueW + typeW), 0)
                .size(remarkW, headerH).text("备注").fontSize(kFontLabel).lineHeight(headerH).color(theme.metaText).build();
        }
        drawEditorDivider(ui, id + ".header.divider", 0, headerH - kEditorDividerHeight, w, theme);
    }).build();

    const std::vector<std::string> types = {"string", "integer", "number", "boolean"};
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const std::string rowId = id + ".row." + std::to_string(i);
        const float rowY = y + headerH + static_cast<float>(i) * rowH;
        ui.stack(rowId).position(x, rowY).size(w, rowH).content([&] {
            ui.stack(rowId + ".enabled.wrap").position(0, 0).size(enabledW, rowH).content([&] {
                components::checkbox(ui, rowId + ".enabled").size(enabledW, 24.0f)
                    .checked(items[i].enabled).theme(tokens)
                    .style(checkboxStyle(theme))
                    .onChange([&items, i](bool value) { items[i].enabled = value; }).build();
            }).build();
            components::input(ui, rowId + ".key").position(enabledW + gap, 2.0f).size(keyW, 24.0f)
                .value(items[i].key).placeholder("Key").theme(tokens)
                .onChange([&items, i](const std::string& value) { items[i].key = value; }).build();
            components::input(ui, rowId + ".value").position(enabledW + gap * 2 + keyW, 2.0f).size(valueW, 24.0f)
                .value(items[i].value).placeholder("Value").theme(tokens)
                .onChange([&items, i](const std::string& value) { items[i].value = value; }).build();
            if (showMeta) {
                const std::string typeId = rowId + ".type";
                ui.stack(typeId + ".wrap").position(enabledW + gap * 3 + keyW + valueW, 2.0f).size(typeW, 24.0f)
                    .zIndex(30).content([&] {
                        registerSelectionPopup(typeId, g_paramTypeOpen[rowId], [key = rowId] { g_paramTypeOpen[key] = false; });
                        components::dropdown(ui, typeId).size(typeW, 24.0f).items(types)
                            .selected([&] { const auto it = std::ranges::find(types, items[i].type); return it == types.end() ? 0 : static_cast<int>(it - types.begin()); }())
                            .open(g_paramTypeOpen[rowId]).theme(tokens)
                            .onOpenChange([key = rowId](bool open) { g_paramTypeOpen[key] = open; setSelectionPopupOpen(key + ".type", open); })
                            .onChange([&items, i, types, key = rowId](int selected) { items[i].type = types[std::clamp(selected, 0, static_cast<int>(types.size()) - 1)]; g_paramTypeOpen[key] = false; })
                            .build();
                    }).build();
                components::input(ui, rowId + ".remark").position(enabledW + gap * 4 + keyW + valueW + typeW, 2.0f).size(remarkW, 24.0f)
                    .value(items[i].remark).placeholder("备注").theme(tokens)
                    .onChange([&items, i](const std::string& value) { items[i].remark = value; }).build();
            }
            components::button(ui, rowId + ".delete").position(w - actionW, 4.0f).size(actionW, actionW)
                .icon(0xF00D).text("").iconSize(9.0f).theme(tokens, false).radius(kIconButtonRadius)
                .onClick([&items, i] { items.erase(items.begin() + i); }).build();
            drawEditorDivider(ui, rowId + ".divider", 0, rowH - kEditorDividerHeight, w, theme);
        }).build();
    }
    const float addY = y + headerH + static_cast<float>(items.size()) * rowH;
    components::button(ui, id + ".add").position(x, addY).size(64.0f, kCompactButtonHeight)
        .icon(0xF067).text("添加").fontSize(kFontLabel).theme(tokens, false).radius(kButtonRadius)
        .onClick([&items, showMeta] { items.push_back({.enabled = true, .type = showMeta ? "string" : ""}); }).build();
    return addY + kCompactButtonHeight;
}

export float drawParamEditor(eui::Ui& ui, const std::string& id, float x, float y, float w,
                             std::vector<api::KeyValue>& items, const AppTheme& theme) {
    const float delW = kIconButtonSize;
    const float gap = kEditorRowGap;
    const float rowH = kInputHeight * 2.0f + gap;
    // 单行最小跨度 = 96+96+92+80+22+4*gap = 402；低于阈值切换到两行布局：
    // 第一行 key/value/type，第二行 remark + 删除，所有宽度随 w 收缩不为负。
    const bool wide = w >= 402.0f;
    const float keyW = wide ? std::max(96.0f, (w - delW - gap * 4.0f) * 0.25f)
                            : nonNegative(w - 92.0f - gap * 2.0f) * 0.5f;
    const float valueW = wide ? std::max(96.0f, (w - delW - gap * 4.0f) * 0.25f)
                              : nonNegative(w - 92.0f - gap * 2.0f) - keyW;
    const float typeW = wide ? 92.0f : std::min(92.0f, nonNegative(w - gap * 2.0f));
    const float remarkW = wide
        ? std::max(80.0f, w - keyW - valueW - typeW - delW - gap * 4.0f)
        : nonNegative(w - delW - gap);
    const std::vector<std::string> types = {"string", "integer", "number", "boolean"};

    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const std::string rowId = id + ".param." + std::to_string(i);
        const float remarkY = wide ? 0.0f : kInputHeight + gap;
        const float delY = wide ? 2.0f : kInputHeight + gap + 2.0f;
        ui.stack(rowId)
            .position(x, y)
            .size(w, rowH)
            .content([&] {
                components::input(ui, rowId + ".k")
                    .position(0, 0).size(keyW, kEditorRowHeight)
                    .value(items[i].key).placeholder("Key").theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].key = v; }).build();
                components::input(ui, rowId + ".v")
                    .position(keyW + gap, 0).size(valueW, kInputHeight)
                    .value(items[i].value).placeholder("Value").theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].value = v; }).build();
                ui.stack(rowId + ".type.wrap")
                    .position(keyW + valueW + gap * 2.0f, 0)
                    .size(typeW, kInputHeight)
                    .zIndex(30)
                    .content([&] {
                        const std::string typeId = rowId + ".type";
                        registerSelectionPopup(typeId, g_paramTypeOpen[rowId],
                                                [key = rowId] { g_paramTypeOpen[key] = false; });
                        components::dropdown(ui, typeId)
                            .size(typeW, kInputHeight)
                            .items(types)
                            .selected([&] {
                                const auto it = std::ranges::find(types, items[i].type);
                                return it == types.end() ? 0 : static_cast<int>(it - types.begin());
                            }())
                            .open(g_paramTypeOpen[rowId])
                            .theme(theme.components)
                            .onOpenChange([key = rowId](bool open) {
                                g_paramTypeOpen[key] = open;
                                setSelectionPopupOpen(key + ".type", open);
                            })
                            .onChange([&items, i, types, key = rowId](int selected) {
                                items[i].type = types[std::clamp(selected, 0, static_cast<int>(types.size()) - 1)];
                                g_paramTypeOpen[key] = false;
                            })
                            .build();
                    })
                    .build();
                components::input(ui, rowId + ".remark")
                    .position(wide ? keyW + valueW + typeW + gap * 3.0f : 0.0f, remarkY)
                    .size(remarkW, kInputHeight)
                    .value(items[i].remark).placeholder("备注").theme(theme.components)
                    .onChange([&items, i](const std::string& v) { items[i].remark = v; }).build();
                components::button(ui, rowId + ".del")
                    .position(nonNegative(w - delW), delY).size(delW, delW)
                    .icon(0xF00D).text("").iconSize(9.0f)
                    .theme(theme.components, false)
                    .radius(kIconButtonRadius)
                    .onClick([&items, i] { items.erase(items.begin() + i); }).build();
                drawEditorDivider(ui, rowId + ".divider", 0,
                                  rowH - kEditorDividerHeight, w, theme);
            })
            .build();
    }
    components::button(ui, id + ".param.add")
        .position(x, y).size(64.0f, kCompactButtonHeight)
        .icon(0xF067).text("添加").fontSize(kFontLabel)
        .theme(theme.components, false)
        .radius(kButtonRadius)
        .onClick([&items] { items.push_back({.type = "string"}); }).build();
    return y + items.size() * (rowH + kGap) + 22.0f;
}

// 测量换行文本的真实高度（含 \n 分段与 maxWidth 软换行），走 EUI 的文本尺寸缓存。
// scrollView 的可变文本内容必须用这个高度作为行高，不能用 viewport 高度冒充。
export float measureWrappedTextHeight(const std::string& text, float maxWidth,
                                      float fontSize, const std::string& fontFamily = {},
                                      float lineHeight = 0.0f) {
    core::TextStyle style;
    style.text = text;
    style.fontFamily = fontFamily;
    style.fontSize = fontSize;
    style.maxWidth = std::max(1.0f, maxWidth);
    style.wrap = true;
    style.lineHeight = lineHeight;
    return core::TextPrimitive::measureTextSize(style).y;
}

// dataTable 单元格文本不逐格裁剪（EUI 只按 columnWidth-2*textInset 布局尺寸，
// 不截断超长文本），长 URL / 错误信息会溢出进下一列。这里按可用宽度二分截断
// 补省略号（只在 UTF-8 字符边界上切）。maxWidth <= 0 或省略号都放不下时给空串。
export std::string fitTextToWidth(const std::string& text, float maxWidth,
                                  float fontSize, const std::string& fontFamily = {}) {
    if (text.empty() || maxWidth <= 0.0f) return {};
    auto widthOf = [&](const std::string& s) {
        core::TextStyle style;
        style.text = s;
        style.fontFamily = fontFamily;
        style.fontSize = fontSize;
        style.maxWidth = 1.0e9f;  // 不换行，量原始宽度
        style.wrap = false;
        return core::TextPrimitive::measureTextSize(style).x;
    };
    if (widthOf(text) <= maxWidth) return text;
    const std::string ell = "…";
    if (widthOf(ell) > maxWidth) return {};
    // 先收集每个 UTF-8 字符的起始下标，二分只在字符数上进行：按字节中点再
    // 吸附边界的写法，吸附可能越过 hi 导致区间不再收缩（死循环，已在 560px
    // 窗口压测记录表头实测复现）。
    std::vector<std::size_t> bounds;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) bounds.push_back(i);
    }
    // k 个字符的前缀终点：第 k+1 个字符的起点（k == 字符数时为 size）。
    auto prefixEnd = [&](std::size_t k) {
        return k < bounds.size() ? bounds[k] : text.size();
    };
    std::size_t best = 0;  // 不变式：best 个字符 + 省略号放得下（0 个已验证）
    std::size_t lo = 1;
    std::size_t hi = bounds.size();
    while (lo <= hi) {
        const std::size_t k = lo + (hi - lo) / 2;
        if (widthOf(text.substr(0, prefixEnd(k)) + ell) <= maxWidth) {
            best = k;
            lo = k + 1;
        } else {
            hi = k - 1;
        }
    }
    return text.substr(0, prefixEnd(best)) + ell;
}

export void drawStatusBar(eui::Ui& ui, float width, float height,
                          const std::string& message, const AppTheme& theme) {
    (void)ui;
    (void)width;
    (void)height;
    (void)message;
    (void)theme;
}

// 确认对话框（删除环境/清空历史等）：EUI dialog 设了 content 就不画标题，且其内置
// 主按钮没有 textColor 覆写 —— 本项目主色是白，白底白字不可读。所以标题/消息都在
// content 里自己画，主按钮文字反色（同 drawInputDialog 的 footer 处理）。
export void drawConfirmDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme,
                              const std::string& id, const std::string& title,
                              const std::string& message, const std::string& confirmLabel,
                              std::function<void()> onCancel, std::function<void()> onConfirm) {
    const auto& tokens = theme.components;
    const float dlgW = dialogWidth(screen.width, 380.0f);
    const float dlgH = dialogHeight(screen.height, 176.0f);
    const float pad = 20.0f;
    const float btnW = 74.0f;
    const float btnH = 24.0f;
    const float btnY = nonNegative(dlgH - btnH - 26.0f);
    const float confirmX = nonNegative(dlgW - pad - btnW);
    const float cancelX = nonNegative(confirmX - 8.0f - btnW);
    const float msgY = 52.0f;
    const float msgH = nonNegative(btnY - msgY - 10.0f);
    components::dialog(ui, id)
        .open(true).screen(screen.width, screen.height).size(dlgW, dlgH)
        .title(title).theme(tokens)
        .content([&] {
            ui.text(id + ".heading")
                .position(pad, 14.0f).size(nonNegative(dlgW - pad * 2.0f), 24.0f)
                .text(title).fontSize(kFontBody + 1.0f).lineHeight(24.0f)
                .color(theme.titleText)
                .verticalAlign(core::VerticalAlign::Center).build();
            if (msgH > 0.0f) {
                ui.text(id + ".msg")
                    .position(pad, msgY).size(nonNegative(dlgW - pad * 2.0f), msgH)
                    .text(message).fontSize(kFontBody).color(theme.bodyText)
                    .wrap(true).build();
            }
            components::button(ui, id + ".cancel")
                .position(cancelX, btnY).size(btnW, btnH)
                .text("取消").fontSize(kFontLabel).theme(tokens, false)
                .radius(kButtonRadius)
                .onClick(std::move(onCancel)).build();
            components::button(ui, id + ".confirm")
                .position(confirmX, btnY).size(btnW, btnH)
                .text(confirmLabel).fontSize(kFontLabel).theme(tokens, true)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                .radius(kButtonRadius)
                .onClick(std::move(onConfirm)).build();
        }).build();
}

// 单行输入对话框（重命名/新建等）：尺寸经 dialogWidth/dialogHeight clamp 到
// 屏幕内，footer 按实际 dialog 宽度右对齐，不再使用固定 360x154 绝对坐标。
export void drawInputDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme,
                            const std::string& id, const std::string& title,
                            std::string& value, const std::string& placeholder,
                            const std::string& confirmLabel,
                            std::function<void()> onCancel, std::function<void()> onConfirm) {
    const auto& tokens = theme.components;
    const float dlgW = dialogWidth(screen.width, 360.0f);
    const float dlgH = dialogHeight(screen.height, 154.0f);
    const float pad = 20.0f;
    const float btnW = 74.0f;
    const float btnH = 24.0f;
    const float btnY = nonNegative(dlgH - btnH - 26.0f);
    const float confirmX = nonNegative(dlgW - pad - btnW);
    const float cancelX = nonNegative(confirmX - 8.0f - btnW);
    const float inputY = std::min(56.0f, nonNegative(btnY - kInputHeight - 14.0f));
    components::dialog(ui, id)
        .open(true).screen(screen.width, screen.height).size(dlgW, dlgH)
        .title(title).theme(tokens)
        .content([&] {
            // content 路径下 EUI 不画 .title()，标题在 content 里自己画。
            ui.text(id + ".heading")
                .position(pad, 14.0f).size(nonNegative(dlgW - pad * 2.0f), 24.0f)
                .text(title).fontSize(kFontBody + 1.0f).lineHeight(24.0f)
                .color(theme.titleText)
                .verticalAlign(core::VerticalAlign::Center).build();
            components::input(ui, id + ".input")
                .position(pad, inputY).size(nonNegative(dlgW - pad * 2.0f), kInputHeight)
                .value(value).placeholder(placeholder).theme(tokens)
                .onChange([&value](const std::string& v) { value = v; }).build();
            components::button(ui, id + ".cancel")
                .position(cancelX, btnY).size(btnW, btnH)
                .text("取消").fontSize(kFontLabel).theme(tokens, false)
                .radius(kButtonRadius)
                .onClick(std::move(onCancel)).build();
            components::button(ui, id + ".confirm")
                .position(confirmX, btnY).size(btnW, btnH)
                .text(confirmLabel).fontSize(kFontLabel).theme(tokens, true)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                .radius(kButtonRadius)
                .onClick(std::move(onConfirm)).build();
        }).build();
}
