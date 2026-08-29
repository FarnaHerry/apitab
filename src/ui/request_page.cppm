// request_page.cppm — 请求工作区骨架：方法 / URL / 发送 / 响应查看。
// 发送走领域 store（g_requests.send），TaskScope 里轮询结果并直接写 State。
module;

#include <huxerui/huxerui.h>

export module apitab.ui.request_page;

import std;
import apitab.api_engine;
import apitab.store.requests;
import apitab.store.ui;
import apitab.ui.common;

export namespace apitab::ui {


[[huxerui::composable]] inline huxerui::View RequestPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    constexpr std::array<std::string_view, 7> kMethodNames{
        "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};
    (void)kMethodNames;

    auto methodIndex = huxerui::UseState<std::size_t>(0);
    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    auto responseText = huxerui::UseState(std::string{"（尚未发送请求）"});
    auto inFlight = huxerui::UseState(false);

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("请求", "单次 API 调试（curl 引擎）"),
        // 方法常量
        huxerui::SegmentedButton({"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"},
                                 methodIndex)
            .OnChanged([methodIndex](std::size_t index) { methodIndex = index; }),
        huxerui::TextField(url)
            .Label("URL")
            .Placeholder("https://api.example.com/v1/resource")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; }),
        huxerui::Row {
            huxerui::Button("发送（骨架）").OnClick([=] {}),
        },
        huxerui::Text(inFlight.Get() ? "● 进行中" : "○ 空闲", huxerui::TextRole::Label),
        huxerui::Text(responseText.Get(), huxerui::TextRole::Body)
            .With(huxerui::Frame(std::nullopt, 320.0F))
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))};
}

} // namespace apitab::ui
