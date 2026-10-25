#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>

namespace apitab::ui {

// 与页面内容分开持有卡片的外轮廓，供主窗口、子窗口和布局回归共用。
inline huxerui::View PageCardSurface(huxerui::View content, const huxerui::ThemeSpec& theme,
                                    huxerui::Color lightBorderColor) {
    constexpr huxerui::Color kDarkPageCardColor = huxerui::Color::Rgb(48, 48, 48);  // #303030
    constexpr huxerui::Color kLightPageCardColor = huxerui::Color::Rgb(246, 248, 250);  // #f6f8fa
    constexpr float kPageCardBorderWidth = 1.0F;
    const bool isDarkTheme = theme.colors.background.red < 0.5F;
    const huxerui::Color cardColor = isDarkTheme ? kDarkPageCardColor : kLightPageCardColor;
    const huxerui::Color cardBorderColor =
        isDarkTheme ? huxerui::Color::White() : lightBorderColor;
    // Border 只参与绘制，不会自动内缩内容。为它预留完整线宽，并把页面裁剪到
    // 同心的内圆角，防止不透明工作面覆盖四边或圆角处的外轮廓。
    huxerui::View body = huxerui::Column {
      content,
    }.With(
        huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
        huxerui::CornerRadius(std::max(0.0F, theme.shapes.small - kPageCardBorderWidth)),
        huxerui::ClipChildren());
    huxerui::View card = huxerui::Column {
      body,
    }.With(
        huxerui::Padding(kPageCardBorderWidth),
        huxerui::Grow(1.0F),
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
        huxerui::Background(cardColor),
        huxerui::CornerRadius(theme.shapes.small),
        huxerui::ClipChildren(),
        // 外轮廓比内部 0.5dp 分隔线略重，避免缩放后难以辨认。
        huxerui::Border(cardBorderColor, kPageCardBorderWidth));
    // 卡片与侧栏及窗口边缘留出小间距，内部页面继续用细线表达分区。
    return huxerui::Column {
      card,
    }.With(huxerui::Padding(theme.spacing.small),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
