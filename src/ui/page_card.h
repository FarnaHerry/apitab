#pragma once

#include <huxerui/huxerui.h>

#include <optional>
#include <utility>

namespace apitab::ui {

// 与页面内容分开持有卡片的外轮廓，供主窗口、子窗口和布局回归共用。
inline huxerui::View PageCardSurface(huxerui::View content, const huxerui::ThemeSpec& theme,
                                    huxerui::Color lightBorderColor,
                                    std::optional<huxerui::View> footer = std::nullopt) {
    constexpr huxerui::Color kDarkPageCardColor = huxerui::Color::Rgb(48, 48, 48);  // #303030
    constexpr huxerui::Color kLightPageCardColor = huxerui::Color::Rgb(246, 248, 250);  // #f6f8fa
    constexpr float kPageCardBorderWidth = 0.3F;
    const float cardRadius = theme.shapes.large;
    const bool isDarkTheme = theme.colors.background.red < 0.5F;
    const huxerui::Color cardColor = isDarkTheme ? kDarkPageCardColor : kLightPageCardColor;
    const huxerui::Color cardBorderColor =
        isDarkTheme ? huxerui::Color::White() : lightBorderColor;
    if (footer) {
        content = huxerui::Column {
          std::move(content).With(huxerui::Grow(1.0F)),
          *footer,
        }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    // Stack 按声明顺序绘制：内容先画，纯描边层最后画，不会被不透明工作面覆盖。
    // 描边层不绑定事件、焦点或语义，保持下面控件的点击、输入和滚动路由。
    huxerui::View card = huxerui::Stack {
      content,
      huxerui::Row{}.With(huxerui::Border(cardBorderColor, kPageCardBorderWidth),
                          huxerui::CornerRadius(cardRadius)).Key("page-card-outline"),
    }.With(
        huxerui::Grow(1.0F),
        huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch),
        huxerui::Background(cardColor),
        huxerui::CornerRadius(cardRadius),
        huxerui::ClipChildren());
    // 卡片与侧栏及窗口边缘留出小间距，内部页面继续用细线表达分区。
    return huxerui::Column {
      card,
    }.With(huxerui::Padding(theme.spacing.small),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
