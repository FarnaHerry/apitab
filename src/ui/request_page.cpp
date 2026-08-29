// request_page.cpp — 请求工作区：方法 / URL / Params / Headers / Cookies / Body
// 编辑、发送（TaskScope 轮询 curl 引擎）、响应 Body/Headers 查看、保存到集合。
#include <huxerui/huxerui.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"
#include "app_resources.h"

import apitab.api_engine;
import apitab.db;
import apitab.preferences;
import apitab.store.requests;

namespace apitab::ui {

namespace {
constexpr std::array<std::string_view, 7> kMethodNames{
    "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS"};

// 下标与 api::BodyKind 一一对应（None=0 … GraphQL=6）。
constexpr std::array<std::string_view, 7> kBodyTypeNames{
    "无", "JSON", "Text", "Form URL-Encoded", "Form-Data", "XML", "GraphQL"};

// 通用下拉选择器：触发按钮显示当前项 + ▾，点击弹出锚定菜单（SDK 无原生 ComboBox）。
[[huxerui::composable]] huxerui::View DropdownSelect(std::vector<std::string> items,
                                                     huxerui::State<std::size_t> index) {
    auto menu = huxerui::UseMenu();
    const std::size_t current = index.Get() < items.size() ? index.Get() : 0;
    return huxerui::Button(items.at(current) + " ▾")
        .OnClick([menu, items, index] {
            // 菜单项回调在菜单层关闭后才执行，已脱离指针事件路径，可直接写 State。
            std::vector<huxerui::MenuEntry> entries;
            entries.reserve(items.size());
            for (std::size_t i = 0; i < items.size(); ++i) {
                huxerui::MenuItem item(items[i], [index, i] { index = i; });
                if (i == index.Get())
                    entries.push_back(std::move(item).Checked(true));
                else
                    entries.push_back(std::move(item));
            }
            menu.Show(std::move(entries),
                      huxerui::MenuOptions{
                          .placement = {huxerui::AnchorSide::Below,
                                        huxerui::AnchorAlignment::Start}});
        })
        .With(menu.Anchor());
}

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
    auto tasks = huxerui::UseTaskScope();
    std::vector<huxerui::View> children{huxerui::Text(keyLabel, huxerui::TextRole::Label)};

    const std::vector<KvRow> snapshot = rows.Get();
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(snapshot[i].enabled).OnChanged([rows, i](bool checked) {
                    std::vector<KvRow> copy = rows.Get();
                    if (i < copy.size()) copy[i].enabled = checked;
                    rows = copy;
                }),
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
                huxerui::Button("✕").OnClick([tasks, rows, i] {
                    // 删除会移除本按钮所在行：推迟出指针事件路径
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        std::vector<KvRow> copy = rows.Get();
                        if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                        rows = copy;
                    });
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

    const huxerui::TextStyle mono{.font = huxerui::Font::Monospace(13.0F),
                                  .foreground = theme.colors.on_surface};
    std::vector<huxerui::View> children{
        huxerui::SegmentedButton({"Body", "Headers"}, responseTab)
            .OnChanged([responseTab](std::size_t index) { responseTab = index; })};
    if (responseTab.Get() == 0) {
        children.push_back(huxerui::SelectionArea{
            huxerui::Text(responseBody.Get(), huxerui::TextRole::Body).Style(mono)});
    } else {
        const std::vector<std::string> lines = responseHeaders.Get();
        std::vector<huxerui::View> rows;
        rows.reserve(lines.size());
        for (const std::string& line : lines)
            rows.push_back(huxerui::Text(line, huxerui::TextRole::Body).Style(mono));
        children.push_back(
            rows.empty()
                ? huxerui::View{huxerui::Text("（无响应头）", huxerui::TextRole::Body)}
                : huxerui::View{huxerui::SelectionArea{huxerui::Column(std::move(rows))
                                                           .With(huxerui::Spacing(2.0F))}});
    }

    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

[[huxerui::composable]] huxerui::View RequestPage(huxerui::State<std::int64_t> opened) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto navPage = huxerui::UseState<std::size_t>(1);

    // 当前项目名（打开工作区时由主页写入领域 store）。
    std::string projectName = "（未知项目）";
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == opened.Get()) projectName = p.name;
    }

    // 未打开项目：完整空状态页（图标 + 说明 + 去主页按钮）。
    if (opened.Get() == 0) {
        return huxerui::ScrollView{EmptyState(app::images::request, "请求工作区未打开",
                                              "先在主页打开一个项目，再进入请求、压测等页面。",
                                              navPage)}
            .With(huxerui::ScrollBar());
    }

    auto methodIndex = huxerui::UseState<std::size_t>(0);
    auto url = huxerui::UseState(huxerui::TextEditingValue{});
    auto section = huxerui::UseState<std::size_t>(0); // 0=Params 1=Headers 2=Cookies 3=Body
    auto params = huxerui::UseState<std::vector<KvRow>>({});
    auto headers = huxerui::UseState<std::vector<KvRow>>({});
    auto cookies = huxerui::UseState<std::vector<KvRow>>({});
    auto bodyKindIndex = huxerui::UseState<std::size_t>(0); // 下标 = api::BodyKind 值
    auto body = huxerui::UseState(huxerui::TextEditingValue{});
    auto bodyFields = huxerui::UseState<std::vector<KvRow>>({}); // Form 类 body 的字段
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
        spec.bodyKind = static_cast<api::BodyKind>(bodyKindIndex.Get());
        switch (spec.bodyKind) {
            case api::BodyKind::None:
                break;
            case api::BodyKind::FormUrlEncoded:
            case api::BodyKind::FormData:
                for (const KvRow& row : bodyFields.Get())
                    if (row.enabled && !row.key.text.empty())
                        spec.bodyFields.push_back(ToKeyValue(row));
                break;
            default: // Json/Text/Xml/GraphQL：文本体
                spec.body = body.Get().text;
                break;
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
        default: {
            // Body：先选类型（下标 = api::BodyKind），再按类型展开对应编辑器。
            std::vector<huxerui::View> bodyChildren{
                DropdownSelect(
                    std::vector<std::string>(kBodyTypeNames.begin(), kBodyTypeNames.end()),
                    bodyKindIndex)};
            switch (bodyKindIndex.Get()) {
                case 0: // 无
                    bodyChildren.push_back(
                        huxerui::Text("无请求体", huxerui::TextRole::Body)
                            .With(huxerui::Foreground(theme.colors.on_surface_variant)));
                    break;
                case 3: // Form URL-Encoded
                case 4: // Form-Data
                    bodyChildren.push_back(KvTable(bodyFields, theme, "字段名", "字段值"));
                    break;
                default: // JSON/Text/XML/GraphQL
                    bodyChildren.push_back(
                        huxerui::TextField(body)
                            .Label("Body（" +
                                   std::string{kBodyTypeNames.at(bodyKindIndex.Get())} + "）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .LineLimits(huxerui::TextFieldLineLimits::MultiLine(4, 12))
                            .OnChanged([body](const huxerui::TextEditingValue& value) {
                                body = value;
                            }));
                    break;
            }
            editorChildren.push_back(huxerui::Column(std::move(bodyChildren))
                                         .With(huxerui::Spacing(theme.spacing.small),
                                               huxerui::CrossAlign(
                                                   huxerui::CrossAxisAlignment::Stretch)));
            break;
        }
    }

    return huxerui::ScrollView{huxerui::Column {
        PageHeader("请求", "项目: " + projectName + " · 单次 API 调试（curl 引擎）"),
        huxerui::Button("关闭工作区").OnClick([tasks, opened] {
            // 关闭会卸载本按钮所在页：推迟出指针事件路径
            tasks.Launch([opened]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                saveSessionPreference("active_project", "0");
                opened = 0;
            });
        }),
        huxerui::Row {
            DropdownSelect(
                std::vector<std::string>(kMethodNames.begin(), kMethodNames.end()), methodIndex),
            huxerui::TextField(url)
                .Label("URL")
                .Placeholder("https://api.example.com/v1/resource")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; })
                .With(huxerui::Grow(1.0F)),
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
                .OnChanged([saveName](const huxerui::TextEditingValue& value) { saveName = value; })
                .With(huxerui::Frame{.width = 160.0F}),
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
                // form 类 body 的结构化字段按类型落进 bodyContents，避免保存后丢失。
                const std::size_t bodyIndex = static_cast<std::size_t>(spec.bodyKind);
                if (bodyIndex < saved.bodyContents.size()) {
                    saved.bodyContents[bodyIndex].text = spec.body;
                    saved.bodyContents[bodyIndex].fields = spec.bodyFields;
                }
                if (const std::string err = g_requests.save(saved); !err.empty())
                    toast.Show("保存失败: " + err);
                else
                    toast.Show("已保存到集合");
            }),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Column(std::move(editorChildren)).With(huxerui::Spacing(theme.spacing.medium)),
        ResponseArea(responseBody, responseHeaders, theme),
    }
                               .With(huxerui::Padding(theme.spacing.large),
                                     huxerui::Spacing(theme.spacing.medium))}.With(huxerui::ScrollBar());
}

} // namespace apitab::ui
