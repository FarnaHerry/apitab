#pragma once

#include <huxerui/huxerui.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace apitab::ui {

// 测量结果只保存普通几何值，供拖拽事件读取；不在布局阶段读写 State 或保存节点引用。
struct SplitPaneMetrics {
    float extent = 0.0F;
    float minimum = 0.0F;
    float maximum = 0.0F;
};

struct SplitPaneOptions {
    using Value = SplitPaneOptions;
    huxerui::Axis axis = huxerui::Axis::Horizontal;
    float extent = 260.0F;
    float minimum = 180.0F;
    float maximum = 420.0F;
    float reserve = 480.0F;
    bool trailing = false;
    std::shared_ptr<SplitPaneMetrics> metrics;
};

// Row/Column 无法在测量时把固定分区限幅为“父空间 - 拖条 - 对侧保留空间”。
// 此布局仅负责该分配算法；内容、滚动、拖拽输入及 State 仍由普通组件持有。
class SplitPane final : public huxerui::Layout<SplitPane> {
public:
    using Layout::Layout;
    static constexpr float kHandleExtent = 6.0F;

    static huxerui::LayoutResult Measure(huxerui::LayoutContext& context, huxerui::ViewNode& node,
                                        huxerui::Constraints constraints) {
        if (node.ChildCount() != 3) throw std::logic_error("SplitPane requires two panes and one handle");
        const auto options = node.LayoutValueOr<SplitPaneOptions>(SplitPaneOptions{});
        const bool horizontal = options.axis == huxerui::Axis::Horizontal;
        const bool bounded = horizontal ? constraints.HasBoundedWidth() : constraints.HasBoundedHeight();
        const float available = horizontal ? constraints.max_width : constraints.max_height;
        const float handle = bounded ? std::min(kHandleExtent, available) : kHandleExtent;
        const float capacity = bounded ? std::max(0.0F, available - handle) : 0.0F;
        const float maximum = std::max(0.0F, bounded ? std::min(options.maximum, capacity - options.reserve)
                                                    : options.maximum);
        const float minimum = std::min(std::max(0.0F, options.minimum), maximum);
        const float extent = std::clamp(options.extent, minimum, maximum);
        if (options.metrics) *options.metrics = {extent, minimum, maximum};

        const auto paneConstraints = [&](float mainExtent, bool exact) {
            auto child = constraints.Loose();
            if (horizontal) {
                if (exact) child.min_width = child.max_width = mainExtent;
                if (constraints.HasBoundedHeight()) child.min_height = child.max_height;
            } else {
                if (exact) child.min_height = child.max_height = mainExtent;
                if (constraints.HasBoundedWidth()) child.min_width = child.max_width;
            }
            return child;
        };

        const float leadingExtent = options.trailing ? capacity - extent : extent;
        const float trailingExtent = options.trailing ? extent : capacity - extent;
        const auto leading = context.Measure(
            node.ChildAt(0), paneConstraints(leadingExtent, bounded || !options.trailing));
        const auto divider = context.Measure(node.ChildAt(1), paneConstraints(handle, true));
        const auto trailing = context.Measure(
            node.ChildAt(2), paneConstraints(trailingExtent, bounded || options.trailing));
        const float leadingMain = horizontal ? leading.width : leading.height;
        const float dividerMain = horizontal ? divider.width : divider.height;
        const float trailingMain = horizontal ? trailing.width : trailing.height;
        const float cross = horizontal ? std::max({leading.height, divider.height, trailing.height})
                                       : std::max({leading.width, divider.width, trailing.width});
        const float main = leadingMain + dividerMain + trailingMain;
        huxerui::LayoutResult result;
        result.Place(node.ChildAt(0), {});
        result.Place(node.ChildAt(1), horizontal ? huxerui::Point{leadingMain, 0.0F}
                                                : huxerui::Point{0.0F, leadingMain});
        result.Place(node.ChildAt(2), horizontal ? huxerui::Point{leadingMain + dividerMain, 0.0F}
                                                : huxerui::Point{0.0F, leadingMain + dividerMain});
        return result.SetSize(constraints.Constrain(horizontal ? huxerui::Size{main, cross}
                                                               : huxerui::Size{cross, main}));
    }
};

} // namespace apitab::ui
