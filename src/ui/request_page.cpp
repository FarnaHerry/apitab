// request_page.cpp — 请求工作区：方法 / URL / Params / Headers / Cookies / Body
// 编辑、发送（TaskScope 轮询 curl 引擎）、响应 Body/Headers 查看、保存到集合。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.api_engine;
import apitab.db;
import apitab.preferences;
import apitab.store.requests;

namespace apitab::ui {

namespace {
constexpr std::array<std::string_view, 7> kMethodNames{
    "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};

// 受控 KV 行：TextField 保留完整 TextEditingValue。
struct KvRow {
    huxerui::TextEditingValue key;
    huxerui::TextEditingValue value;
    bool enabled = true;

    bool operator==(const KvRow&) const = default;
};

api::KeyValue ToKeyValue(const KvRow& row) {
    return api::KeyValue{.key = row.key.text,
                         .value = row.value.text,
                         .enabled = row.enabled};
}

// KV 编辑表：行渲染 + 增删。rows 以 State<std::vector<KvRow>> 为权威。
[[huxerui::composable]] huxerui::View KvTable(huxerui::State<std::vector<KvRow>> rows,
                                              const huxerui::ThemeSpec& theme,
                                              std::string keyLabel, std::string valueLabel) {
    std::vector<huxerui::View> children{huxerui::Text(keyLabel, huxerui::TextRole::Label)};

    const std::vector<KvRow> snapshot = rows.Get();
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        children.push_back(
            huxerui::Row {
                huxerui::TextField(snapshot[i].key)
                    .Label(keyLabel)
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([rows, i](const huxerui::TextEditingValue& value) {
                        std::vector<KvRow> copy = rows.Get();
                        if (i < copy.size()) copy[i].key = value;
                        rows = copy;
                    }),
                huxerui::TextField(snapshot[i].value)
                    .Label(valueLabel)
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([rows, i](const huxerui::TextEditingValue& value) {
                        std::vector<KvRow> copy = rows.Get();
                        if (i < copy.size()) copy[i].value = value;
                        rows = copy;
                    }),
                huxerui::Checkbox(snapshot[i].enabled).OnChanged([rows, i](bool checked) {
                    std::vector<KvRow> copy = rows.Get();
                    if (i < copy.size()) copy[i].enabled = checked;
                    rows = copy;
                }),
                huxerui::Button("✕").OnClick([rows, i] {
                    std::vector<KvRow> copy = rows.Get();
                    if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                    rows = copy;
                }),
            }
                .With(huxerui::Spacing(theme.spacing.small)));
    }
    children.push_back(huxerui::Button("+ 添加" + keyLabel).OnClick([rows] {
        std::vector<KvRow> copy = rows.Get();
        copy.push_back(KvRow{});
        rows = copy;
    }));

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}
} // namespace

// 响应区：独立重组作用域 —— responseTab/responseBody/responseHeaders 的变化只
// 重组此区域，不扩散到整个请求页（编辑器、KV 表不受影响）。
[[huxerui::composable]] huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                                                   huxerui::State<std::vector<std::string>> responseHeaders,
                                                   const huxerui::ThemeSpec& theme) {
    auto responseTab = huxerui::UseState<std::size_t>(0);

    return huxerui::Column {
        ResponseArea(responseBody, responseHeaders, theme),
    }
        .With(huxerui::Spacing(theme.spacing.medium));
}

[[huxerui::composable]] huxerui::View RequestPage(huxerui::State<std::int64_t> opened) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();

    // 当前项目名（打开工作区时由主页写入领域 store）。
    std::string projectName = "（未知项目）";
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == opened.Get()) projectName = p.name;
    }

    auto methodIndex = huxerui::UseState<std::size_t>(0);
    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    auto section = huxerui::UseState<std::size_t>(0); // 0=Params 1=Headers 2=Cookies 3=Body
    auto params = huxerui::UseState<std::vector<KvRow>>({});
    auto headers = huxerui::UseState<std::vector<KvRow>>({});
    auto cookies = huxerui::UseState<std::vector<KvRow>>({});
    auto body = huxerui::UseState(huxerui::TextEditingValue{});
    auto saveName = huxerui::UseState(huxerui::TextEditingValue{});

    auto inFlight = huxerui::UseState(false);
    auto responseBody = huxerui::UseState(std::string{"（尚未发送请求）"});
    auto responseHeaders = huxerui::UseState<std::vector<std::string>>({});

    // 组装请求规格：params 拼接与全局 Cookie 由领域 store 统一处理。
    auto buildSpec = [=]() -> api::RequestSpec {
        api::RequestSpec spec;
        spec.method = std::string{kMethodNames.at(methodIndex.Get())};
        spec.url = url.Get().text;
        for (const KvRow& row : params.Get())
            if (row.enabled && !row.key.text.empty()) spec.params.push_back(ToKeyValue(row));
        for (const KvRow& row : headers.Get())
            if (row.enabled && !row.key.text.empty()) spec.headers.push_back(ToKeyValue(row));
        for (const KvRow& row : cookies.Get())
            if (row.enabled && !row.key.text.empty()) spec.cookies.push_back(ToKeyValue(row));
        if (!body.Get().text.empty()) {
            spec.bodyKind = api::BodyKind::Text;
            spec.body = body.Get().text;
        }
        return spec;
    };

    std::vector<huxerui::View> editorChildren{huxerui::SegmentedButton(
        {"Params", "Headers", "Cookies", "Body"}, section)
                                      .OnChanged([section](std::size_t index) { section = index; })};
    switch (section.Get()) {
        case 0:
            editorChildren.push_back(KvTable(params, theme, "参数名", "参数值"));
            break;
        case 1:
            editorChildren.push_back(KvTable(headers, theme, "头名称", "头值"));
            break;
        case 2:
            editorChildren.push_back(KvTable(cookies, theme, "Cookie 名", "Cookie 值"));
            break;
        default:
            editorChildren.push_back(
                huxerui::TextField(body)
                    .Label("Body（原样文本）")
                    .Variant(huxerui::TextFieldVariant::Outlined)
                    .LineLimits(huxerui::TextFieldLineLimits::MultiLine(4, 12))
                    .OnChanged([body](const huxerui::TextEditingValue& value) { body = value; }));
            break;
    }

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("请求", "项目: " + projectName + " · 单次 API 调试（curl 引擎）"),
        huxerui::Button("关闭工作区").OnClick([=] {
            auto tasks = huxerui::UseTaskScope();
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                saveSessionPreference("active_project", "0");
                opened = 0;
            });
        }),
        huxerui::SegmentedButton(
            {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"}, methodIndex)
            .OnChanged([methodIndex](std::size_t index) { methodIndex = index; }),
        huxerui::TextField(url)
            .Label("URL")
            .Placeholder("https://api.example.com/v1/resource")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; }),
        huxerui::Column(std::move(editorChildren)).With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Row {
            huxerui::Button(inFlight.Get() ? "发送中…" : "发送").OnClick([=] {
                if (inFlight.Get()) return;
                api::RequestSpec spec = buildSpec();
                if (spec.url.empty()) {
                    toast.Show("URL 不能为空");
                    return;
                }
                inFlight = true;
                responseBody = "发送中…";
                responseHeaders = {};
                g_requests.send(spec, 0);
                tasks.Launch([=]() -> huxerui::Task<void> {
                    api::ResponseView view;
                    while (!g_requests.pollResult(view)) {
                        if (!g_requests.busy()) {
                            responseBody = "请求未完成（引擎空闲）";
                            inFlight = false;
                            co_return;
                        }
                        co_await huxerui::Delay(std::chrono::duration<double>{0.05});
                    }
                    if (view.ok) {
                        responseBody = std::format("HTTP {} · {} · {} bytes\n\n{}", view.status,
                                                   view.totalMs, view.sizeBytes, view.body);
                        std::vector<std::string> lines;
                        for (const api::KeyValue& h : view.headers)
                            lines.push_back(h.key + ": " + h.value);
                        responseHeaders = lines;
                    } else {
                        responseBody = "请求失败: " + view.error;
                    }
                    inFlight = false;
                });
            }),
            huxerui::Button("取消").OnClick([=] { g_requests.cancel(); }),
            huxerui::TextField(saveName)
                .Label("保存为")
                .Placeholder("请求名称")
                .Variant(huxerui::TextFieldVariant::Standard)
                .OnChanged([saveName](const huxerui::TextEditingValue& value) { saveName = value; }),
            huxerui::Button("保存到集合").OnClick([=, buildSpec] {
                if (saveName.Get().text.empty()) {
                    toast.Show("请填写请求名称");
                    return;
                }
                db::SavedRequest saved;
                saved.name = saveName.Get().text;
                api::RequestSpec spec = buildSpec();
                saved.method = spec.method;
                saved.url = spec.url;
                saved.params = spec.params;
                saved.headers = spec.headers;
                saved.cookies = spec.cookies;
                saved.bodyKind = spec.bodyKind;
                saved.body = spec.body;
                if (const std::string err = g_requests.save(saved); !err.empty())
                    toast.Show("保存失败: " + err);
                else
                    toast.Show("已保存到集合");
            }),
        }
            .With(huxerui::Spacing(theme.spacing.medium)),
        ResponseArea(responseBody, responseHeaders, theme),
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))};
}

} // namespace apitab::ui
