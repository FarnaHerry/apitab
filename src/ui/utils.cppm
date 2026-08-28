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
export constexpr float kButtonFontSize = 11.0f;
export constexpr float kCompactButtonHeight = 24.0f;
export constexpr float kCompactButtonFontSize = 10.0f;
export constexpr float kDialogButtonHeight = 28.0f;
export constexpr float kDialogButtonFontSize = 11.0f;
export constexpr float kToolbarButtonSize = 26.0f;
export constexpr float kToolbarIconSize = 13.0f;
export constexpr float kSelectionItemHeight = 22.0f;
export constexpr float kToolbarRowHeight = 24.0f;
export constexpr float kTabHeight = 24.0f;
export constexpr int kPopupZIndex = 100;
export constexpr int kIslandPopupZIndex = 110; // raises the complete island above sibling islands
export constexpr int kPopupHostZIndex = 200;   // root-level host for menus and dropdown islands
export constexpr int kContextMenuZIndex = 220;
export constexpr float kControlGap = 4.0f;
export constexpr float kButtonRadius = 8.0f;      // 输入框、按钮、下拉框统一 8px 圆角
export constexpr float kIconButtonSize = 22.0f;
export constexpr float kIconButtonRadius = kIconButtonSize * 0.5f;
export constexpr float kFontLabel = 10.0f;        // 标签字号
export constexpr float kFontBody = 11.0f;         // 正文字号
export constexpr float kFontMono = 10.0f;         // 等宽（响应体 / 压测输出）
export constexpr float kRowHeight = 24.0f;        // KV 编辑行高
export constexpr float kEditorRowHeight = 22.0f;  // 紧凑字段行高
export constexpr float kEditorRowGap = 2.0f;
export constexpr float kEditorDividerHeight = 1.0f;
export constexpr float kEditorPad = 6.0f;
export constexpr float kPanelRadius = 8.0f;       // 内层面板圆角

// TinyNext 式岛屿布局令牌：所有页面 surface 使用同一套逻辑像素，
// 不在页面里重复散落圆角、内边距和边缘留白。
export constexpr float kRightMargin = 6.0f;
export constexpr float kIslandVInset = 6.0f;
export constexpr float kIslandGap = 2.0f;
export constexpr float kPanelPad = 10.0f;
export constexpr float kIslandRadius = 8.0f;
export constexpr float kCardActionSize = 22.0f;
export constexpr float kCardActionIconSize = 11.0f;

export constexpr float kStatusBarHeight = 20.0f;

export constexpr float kScrollbarWidth = 4.0f;
export constexpr float kScrollbarGap = 6.0f;

export constexpr float kCompactWidth = 720.0f;
export constexpr float kMediumWidth = 960.0f;

export bool isCompact(float width) { return width < kCompactWidth; }
export bool isMedium(float width) { return width < kMediumWidth; }
export float nonNegative(float value) { return std::max(0.0f, value); }

// 父容器 clamp：origin+size 不越过 parentEnd，结果不为负。
// 用于「元素必须留在岛屿/页面内」的所有派生宽高。
export float clampToParent(float origin, float preferred, float parentEnd) {
    return nonNegative(std::min(preferred, parentEnd - origin));
}

export float dialogWidth(float screenWidth, float preferred, float margin = 16.0f) {
    const float available = nonNegative(screenWidth - margin * 2.0f);
    return std::clamp(std::min(preferred, available), 0.0f, available);
}
export float dialogHeight(float screenHeight, float preferred, float margin = 16.0f) {
    const float available = nonNegative(screenHeight - margin * 2.0f);
    return std::clamp(std::min(preferred, available), 0.0f, available);
}
