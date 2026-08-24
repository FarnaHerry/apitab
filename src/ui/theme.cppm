// ui/theme.cppm — dark/light AppTheme + currentTheme()（对齐 tinynext 主题结构，
// 去掉持久化/跟随系统：v1 深色默认，rail 底部按钮手动切换，不落盘）。
//
// eui_neo.h 是头文件（无模块接口），进全局模块片段；只有 eui 类型
// （eui::Color / components::theme tokens）跨模块边界 —— importers 需自行
// include eui 头（ui.* 模块都经 eui_ui.h 这么做）。
module;

#include "eui_ui.h"

#ifndef _WIN32
#include <cstdio>
#endif

export module apitab.ui.theme;

import std;

export enum class ThemeMode { Dark, Light, System };
export ThemeMode g_themeMode = ThemeMode::System;

// 当前 EUI 版本未提供跨平台系统主题读取 API。System 模式使用桌面默认深色，
// 保留为独立模式以便平台 API 可用时无缝接入。
export bool g_dark = true;

// 跟随系统的读取结果在选择该模式时缓存，避免渲染期间启动外部探测进程。
export bool systemDark() {
#ifdef _WIN32
    return true;
#elif defined(__APPLE__)
    return false;
#else
    FILE* pipe = ::popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
    if (!pipe) return true;
    std::string value;
    char buffer[128];
    while (::fgets(buffer, sizeof(buffer), pipe)) value += buffer;
    ::pclose(pipe);
    if (value.find("prefer-light") != std::string::npos) return false;
    return true;
#endif
}


// 全系统颜色都从 currentTheme() 取：控件统一 .theme(theme.components)，
// 裸文本从命名字段取，深浅主题各自定义保证对比度。compose 每帧重跑，切换即时生效。
export struct AppTheme {
    bool dark;
    eui::Color titleText;    // 大标题
    eui::Color bodyText;     // 正文（URL、正文编辑）
    eui::Color metaText;     // 次要文本（时间、大小等）
    eui::Color hintText;     // 空态提示
    eui::Color statusText;   // 底部状态消息
    eui::Color ok;           // 2xx 绿
    eui::Color redirect;     // 3xx 蓝
    eui::Color clientErr;    // 4xx 橙
    eui::Color serverErr;    // 5xx 红 / 传输错误
    eui::Color idle;         // 无状态 灰
    components::theme::ThemeColorTokens components;
};

// HTTP 状态码 → 状态色（响应行着色）。
export eui::Color statusColor(const AppTheme& t, long status) {
    if (status >= 200 && status < 300) return t.ok;
    if (status >= 300 && status < 400) return t.redirect;
    if (status >= 400 && status < 500) return t.clientErr;
    if (status >= 500) return t.serverErr;
    return t.idle;
}

export const AppTheme kDarkTheme = {
    true,
    {0.95f, 0.95f, 0.95f, 1.0f},   // 标题 白
    {0.86f, 0.86f, 0.86f, 1.0f},   // 正文 浅灰
    {0.58f, 0.58f, 0.60f, 1.0f},   // 次要 灰
    {0.42f, 0.42f, 0.44f, 1.0f},   // 空态 灰
    {0.82f, 0.82f, 0.84f, 1.0f},   // 状态消息 浅灰
    {0.55f, 0.82f, 0.60f, 1.0f},   // 2xx 柔和绿
    {0.55f, 0.70f, 0.95f, 1.0f},   // 3xx 柔和蓝
    {0.95f, 0.72f, 0.45f, 1.0f},   // 4xx 柔和橙
    {0.92f, 0.48f, 0.44f, 1.0f},   // 5xx 柔和红
    {0.50f, 0.50f, 0.52f, 1.0f},   // 空闲 灰
    [] {
        auto tokens = components::theme::dark();
        // 近纯黑底 + 纯白主色（对齐 tinynext 简洁极客风）。
        tokens.background = {0.04f, 0.04f, 0.05f, 1.0f};
        tokens.primary = {1.0f, 1.0f, 1.0f, 1.0f};
        // eui input 默认字号 17 未按设计值书写，在 uiScale 下放得过大；覆写为 13。
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

export const AppTheme kLightTheme = {
    false,
    {0.12f, 0.12f, 0.12f, 1.0f},
    {0.18f, 0.18f, 0.18f, 1.0f},
    {0.45f, 0.45f, 0.47f, 1.0f},
    {0.55f, 0.55f, 0.57f, 1.0f},
    {0.15f, 0.15f, 0.16f, 1.0f},
    {0.10f, 0.55f, 0.30f, 1.0f},
    {0.15f, 0.45f, 0.85f, 1.0f},
    {0.85f, 0.55f, 0.10f, 1.0f},
    {0.80f, 0.25f, 0.20f, 1.0f},
    {0.50f, 0.50f, 0.52f, 1.0f},
    [] {
        auto tokens = components::theme::light();
        tokens.primary = {0.0f, 0.0f, 0.0f, 1.0f};
        tokens.background = {0.96f, 0.96f, 0.96f, 1.0f};
        tokens.metrics.typography.input = 13.0f;
        return tokens;
    }(),
};

export const AppTheme& currentTheme() {
    const bool dark = g_themeMode == ThemeMode::Dark ||
                      (g_themeMode == ThemeMode::System && g_dark);
    return dark ? kDarkTheme : kLightTheme;
}
