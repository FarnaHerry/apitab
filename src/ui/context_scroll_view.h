#pragma once

#include "components/scroll.h"
#include "core/dsl.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>

namespace apitab_components {

class ContextScrollViewBuilder {
public:
    ContextScrollViewBuilder(core::dsl::Ui& ui, std::string id)
        : ui_(ui), id_(std::move(id)) {}

    ContextScrollViewBuilder& position(float x, float y) {
        x_ = x;
        y_ = y;
        return *this;
    }
    ContextScrollViewBuilder& size(float width, float height) {
        width_ = width;
        height_ = height;
        return *this;
    }
    ContextScrollViewBuilder& offset(float value) {
        offset_ = std::max(0.0f, value);
        return *this;
    }
    ContextScrollViewBuilder& step(float value) {
        step_ = std::max(1.0f, value);
        return *this;
    }
    ContextScrollViewBuilder& theme(const components::theme::ThemeColorTokens& tokens) {
        style_ = components::ScrollStyle(tokens);
        tokens_ = tokens;
        return *this;
    }
    ContextScrollViewBuilder& onChange(std::function<void(float)> callback) {
        onChange_ = std::move(callback);
        return *this;
    }
    ContextScrollViewBuilder& onContextMenu(
        std::function<void(const core::PointerEvent&, const core::Rect&)> callback) {
        onContextMenu_ = std::move(callback);
        return *this;
    }

    template <typename ComposeFn>
    ContextScrollViewBuilder& content(ComposeFn&& compose) {
        content_ = std::forward<ComposeFn>(compose);
        return *this;
    }

    void build() {
        const float contentHeight = measureContentHeight(width_, height_);
        const bool scrollable = contentHeight > height_;
        const float scrollWidth = scrollable ? 10.0f : 0.0f;
        const float scrollGap = scrollable ? 4.0f : 0.0f;
        const float contentWidth = std::max(0.0f, width_ - scrollWidth - scrollGap);
        const float measuredHeight = scrollable ? measureContentHeight(contentWidth, height_) : contentHeight;
        const float maxOffset = std::max(0.0f, measuredHeight - height_);
        const float currentOffset = std::clamp(offset_, 0.0f, maxOffset);

        ui_.stack(id_)
            .position(x_, y_)
            .size(width_, height_)
            .clip()
            .scrollState(id_, currentOffset, maxOffset, step_)
            .onScrollOffsetChanged(onChange_)
            .onContextMenu(onContextMenu_)
            .content([&] {
                ui_.column(id_ + ".content")
                    .width(contentWidth)
                    .height(core::SizeValue::wrapContent())
                    .scrollContentFrom(id_)
                    .content([&] {
                        if (content_) content_(ui_, contentWidth, height_);
                    })
                    .build();
                if (scrollable) {
                    components::scroll(ui_, id_ + ".scroll")
                        .theme(tokens_)
                        .style(style_)
                        .scrollStateId(id_)
                        .x(std::max(0.0f, width_ - scrollWidth))
                        .size(scrollWidth, height_)
                        .viewport(height_)
                        .content(measuredHeight)
                        .offset(currentOffset)
                        .step(step_)
                        .zIndex(1)
                        .build();
                }
            })
            .build();
    }

private:
    float measureContentHeight(float contentWidth, float viewportHeight) const {
        core::dsl::Ui measureUi;
        measureUi.begin(id_ + ".measure");
        measureUi.column("content")
            .width(contentWidth)
            .height(core::SizeValue::wrapContent())
            .content([&] {
                if (content_) content_(measureUi, contentWidth, viewportHeight);
            })
            .build();
        measureUi.end();
        measureUi.layout(contentWidth, 0.0f);
        const core::dsl::Element* content = measureUi.find("content");
        return content == nullptr ? viewportHeight : std::max(viewportHeight, content->frame.height);
    }

    core::dsl::Ui& ui_;
    std::string id_;
    components::ScrollStyle style_;
    components::theme::ThemeColorTokens tokens_ = components::theme::dark();
    std::function<void(float)> onChange_;
    std::function<void(const core::PointerEvent&, const core::Rect&)> onContextMenu_;
    std::function<void(core::dsl::Ui&, float, float)> content_;
    float width_ = 320.0f;
    float height_ = 220.0f;
    float offset_ = 0.0f;
    float step_ = 40.0f;
    float x_ = 0.0f;
    float y_ = 0.0f;
};

inline ContextScrollViewBuilder contextScrollView(core::dsl::Ui& ui, const std::string& id) {
    return ContextScrollViewBuilder(ui, id);
}

} // namespace apitab_components
