#pragma once

#include <huxerui/huxerui.h>

namespace apitab::ui {

// 只覆盖设置页的组件表面，保留继承样式中的焦点、交互与尺寸配置。
inline huxerui::View SettingsPageTheme(huxerui::View content, const huxerui::ThemeSpec& theme,
                                      huxerui::TextFieldStyle fields, huxerui::SelectStyle selects,
                                      huxerui::SegmentedButtonStyle segments) {
    huxerui::ThemeDefinition styles;
    fields.outlined.background = theme.colors.surface_container;
    styles.Set(fields);
    selects.background = theme.colors.surface_container;
    selects.popup_background = theme.colors.surface_container_highest;
    styles.Set(selects);
    segments.background = theme.colors.surface_container;
    segments.selected_background = theme.colors.surface_container_highest;
    styles.Set(segments);
    // Theme 是环境节点，不能附加 Grow/Frame/事件等 View 行为；布局由 content 持有。
    return huxerui::Theme{styles, content};
}

} // namespace apitab::ui
