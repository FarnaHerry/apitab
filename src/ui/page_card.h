#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace apitab::ui {

// 只填充矩形边界与圆角轮廓之间的四个角，不向子树传播路径裁剪。
inline huxerui::Path PageCardCornerMask(huxerui::Size size, float radius) {
    huxerui::Path path;
    const float r = std::clamp(radius, 0.0F, std::max(0.0F, std::min(size.width, size.height) * 0.5F));
    if (r == 0.0F) return path;
    const float w = size.width;
    const float h = size.height;
    path.MoveTo({0.0F, 0.0F}).LineTo({w, 0.0F}).LineTo({w, h}).LineTo({0.0F, h}).Close();
    path.MoveTo({r, 0.0F}).LineTo({w - r, 0.0F})
        .ArcTo({r, r}, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise, {w, r})
        .LineTo({w, h - r})
        .ArcTo({r, r}, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise, {w - r, h})
        .LineTo({r, h})
        .ArcTo({r, r}, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise, {0.0F, h - r})
        .LineTo({0.0F, r})
        .ArcTo({r, r}, 0.0F, huxerui::ArcSize::Small, huxerui::ArcDirection::Clockwise, {r, 0.0F}).Close();
    return path;
}

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
    // PlatformView 不支持祖先路径裁剪：内容仅做矩形裁剪，圆角由前景遮角绘制。
    // 两个前景层都不绑定事件、焦点或语义，保持下面控件的点击、输入和滚动路由。
    const huxerui::Color canvasColor = theme.colors.background;
    huxerui::View card = huxerui::Stack {
      huxerui::Column {
        content,
      }.With(huxerui::Grow(1.0F), huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
             huxerui::ClipChildren()),
      huxerui::Canvas([cardRadius, canvasColor](huxerui::PaintContext& paint, huxerui::Size size) {
          paint.FillPath(PageCardCornerMask(size, cardRadius), canvasColor, huxerui::PathFillRule::EvenOdd);
      }),
      huxerui::Row{}.With(huxerui::Border(cardBorderColor, kPageCardBorderWidth),
                          huxerui::CornerRadius(cardRadius)).Key("page-card-outline"),
    }.With(
        huxerui::Grow(1.0F),
        huxerui::Align(huxerui::HorizontalAlignment::Stretch, huxerui::VerticalAlignment::Stretch),
        huxerui::Background(cardColor),
        huxerui::CornerRadius(cardRadius));
    // 卡片与侧栏及窗口边缘留出小间距，内部页面继续用细线表达分区。
    return huxerui::Column {
      card,
    }.With(huxerui::Padding(theme.spacing.small),
           huxerui::Grow(1.0F),
           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
