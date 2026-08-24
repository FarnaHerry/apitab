// ui/utils.cppm — UI 布局常量（设计逻辑像素）+ 缩放系数 kUI。
// eui-neo 0.5.6 起 DslAppConfig::uiScale(kUI) 原生放大整个逻辑坐标系，
// 所有尺寸按设计逻辑像素直接书写；窗口物理尺寸 = 设计尺寸 × kUI。
module;

#include "eui_ui.h"

export module apitab.ui.utils;

import std;

export import apitab.utils;  // 纯 string/number 帮助函数转发（UI 模块只 import 本模块即可）

export constexpr float kUI = 1.4f;

// ---- 布局常量（设计逻辑像素）----
export constexpr float kRailWidth = 40.0f;        // 图标导航栏宽
export constexpr float kSidebarWidth = 190.0f;    // 集合列表侧栏宽
export constexpr float kMargin = 8.0f;            // 内容区外边距
export constexpr float kGap = 6.0f;               // 控件间距
export constexpr float kInputHeight = 26.0f;      // 单行输入框高
export constexpr float kButtonHeight = 26.0f;
export constexpr float kFontLabel = 11.0f;        // 标签字号
export constexpr float kFontBody = 12.0f;         // 正文字号
export constexpr float kFontMono = 11.0f;         // 等宽（响应体 / 压测输出）
export constexpr float kRowHeight = 24.0f;        // KV 编辑行高
export constexpr float kPanelRadius = 8.0f;       // 面板圆角
