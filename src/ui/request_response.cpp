// request_response.cpp — 请求工作区右侧下岛：响应区（Body/Headers/Cookies）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 响应展示），纯搬移、行为零变化。
#include <huxerui/huxerui.h>

#include <string>
#include <vector>

#include "ui.h"

namespace apitab::ui {

// 响应区（右侧下岛）：独立重组作用域 —— responseTab/responseBody/responseHeaders/
// responseCookies 的变化只重组此区域，不扩散到整个编辑器（KV 表不受影响）。
// 标题与 Body/Headers/Cookies 切换固定在上，响应内容在内部 ScrollView 里滚动
// （垂直滚动条）。
[[huxerui::composable]] huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                                                   huxerui::State<std::vector<std::string>> responseHeaders,
                                                   huxerui::State<std::vector<std::string>> responseCookies,
                                                   const huxerui::ThemeSpec& theme) {
    auto responseTab = huxerui::UseState<std::size_t>(0);

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(font_size::kMonoBody),
                                  .foreground = theme.colors.on_surface};
    huxerui::View content = huxerui::Row{};
    if (responseTab.Get() == 0) {
        content = huxerui::View{huxerui::SelectionArea{
            huxerui::Text(responseBody.Get(), huxerui::TextRole::Body).Style(mono)}};
    } else if (responseTab.Get() == 1) {
        const std::vector<std::string> lines = responseHeaders.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        content = rows.empty()
            ? huxerui::View{huxerui::Text("（无响应头）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(2.0F))}};
    } else {
        const std::vector<std::string> lines = responseCookies.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        content = rows.empty()
            ? huxerui::View{huxerui::Text("（无 Cookie）", huxerui::TextRole::Body)}
            : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                       .With(huxerui::Spacing(2.0F))}};
    }

    return huxerui::Column {
        huxerui::Row {
            huxerui::Text("响应", huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
            huxerui::SegmentedButton({"Body", "Headers", "Cookies"}, responseTab)
                .OnChanged([responseTab](std::size_t index) { responseTab = index; }),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::ScrollView{std::move(content)}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
