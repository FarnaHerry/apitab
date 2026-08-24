// ui/widgets.cppm — 可复用控件：rail 导航项 / 小节标签 / 方法徽章色 /
// KV 编辑器（Params 与 Headers 共用）/ 底部状态条。
module;

#include "eui_ui.h"

export module apitab.ui.widgets;

import std;
import apitab.api_engine;
import apitab.ui.theme;
import apitab.ui.utils;

// ---- rail 导航项（图标 + 激活高亮，无 tooltip）----

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

// ---- 底部状态条 ----

export void drawStatusBar(eui::Ui& ui, float width, float height,
                          const std::string& message, const AppTheme& theme) {
    ui.rect("statusbar.bg")
        .position(0, height - 20.0f)
        .size(width, 20.0f)
        .color(components::theme::withAlpha(theme.components.surface, 0.6f))
        .build();
    ui.text("statusbar.text")
        .position(kRailWidth + kMargin, height - 20.0f)
        .size(width - kRailWidth - kMargin * 2.0f, 20.0f)
        .text(message)
        .fontSize(kFontLabel)
        .lineHeight(20.0f)
        .color(theme.statusText)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
}
