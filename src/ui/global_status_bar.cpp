// global_status_bar.cpp — 底部全局状态条（P1-C2 自 app.cpp 纯搬移）：
//   TruncateSummary / CookieRow / CookieRowsFromStore / StatusActionText /
//   RequestProxyDialogContent / GlobalCookieDialogContent / GlobalStatusBar。
//   右缘两个文字热区（应用代理 / 项目 Cookie）点击开弹窗；弹窗层捕获调用处环境
//   （AppRoot 在 OceanThemed 之上，UseTheme 须在 provider 之下，故 GlobalStatusBar
//   独立成 composable）。P1-B1 前保持现状，不拆为项目/状态两栏。
#include <huxerui/huxerui.h>

#include <functional>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import apitab.db;
import apitab.preferences;
import apitab.store.requests;
import apitab.store.loadtest;
import apitab.utils;

namespace apitab::ui {

namespace {
// ---- 底部全局状态条与右缘弹窗 ----

// 状态栏摘要按码点截断（代理地址可能含非 ASCII）：数 UTF-8 首字节（非 10xxxxxx
// 续字节），超限截到上一个完整码点并补省略号。
std::string TruncateSummary(const std::string& text, std::size_t maxChars) {
    std::size_t chars = 0;
    std::size_t bytes = 0;
    for (const char ch : text) {
        if ((static_cast<unsigned char>(ch) & 0xC0U) != 0x80U) {
            if (chars >= maxChars) return text.substr(0, bytes) + "…";
            ++chars;
        }
        ++bytes;
    }
    return text; // 未超限：原样返回。
}

// 项目 Cookie 的行编辑缓冲：store 行（id/enabled）+ 完整 TextEditingValue。
struct CookieRow {
    std::int64_t id = 0;
    huxerui::TextEditingValue name;
    huxerui::TextEditingValue value;
    bool enabled = true;

    bool operator==(const CookieRow&) const = default;
};

// 从 store 读当前项目 Cookie 列表 → 编辑缓冲（弹窗打开时取初值）。
std::vector<CookieRow> CookieRowsFromStore() {
    std::vector<CookieRow> rows;
    for (const db::GlobalCookie& cookie : g_requests.globalCookies()) {
        rows.push_back(CookieRow{cookie.id, huxerui::TextEditingValue{cookie.name},
                                 huxerui::TextEditingValue{cookie.value}, cookie.enabled});
    }
    return rows;
}

std::vector<KvRow> CookieKvRows(const std::vector<CookieRow>& rows) {
    std::vector<KvRow> result;
    result.reserve(rows.size());
    for (const CookieRow& row : rows) {
        result.push_back(KvRow{.key = row.name, .value = row.value, .enabled = row.enabled});
    }
    return result;
}

// KvTable 的回调只传值行；Cookie id 保留当前位置。Cookie id 不对 UI 暴露、也不被
// 外部引用，保存时会以最终行集合 upsert 并删除未保留 id，故删除后的位置重排安全。
std::vector<CookieRow> ReconcileCookieRows(const std::vector<CookieRow>& before,
                                           std::vector<KvRow> values) {
    std::vector<CookieRow> result;
    result.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        result.push_back(CookieRow{.id = i < before.size() ? before[i].id : 0,
                                   .name = std::move(values[i].key),
                                   .value = std::move(values[i].value),
                                   .enabled = values[i].enabled});
    }
    return result;
}

// 状态条右缘文字热区：语义图标 + kCaption 小字 + Padding 热区 + Tooltip + 点击开弹窗。
huxerui::View StatusActionText(std::string icon, std::string label, std::string tooltip,
                               const huxerui::ThemeSpec& theme,
                               std::function<void()> onClick) {
    const huxerui::TextStyle style{
        .font = huxerui::Font::System(font_size::kCaption),
        .foreground = theme.colors.on_surface_variant};
    return huxerui::Row {
        huxerui::Text(std::move(icon), huxerui::TextRole::Label).Style(style),
        huxerui::Text(std::move(label), huxerui::TextRole::Label).Style(style),
    }
        .With(huxerui::Spacing(4.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(theme.spacing.small, 2.0F)),
              huxerui::Tooltip(tooltip),
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = tooltip})
        .OnClick(std::move(onClick));
}

// 项目 Cookie 使用与左侧顶级导航相同的单色 SVG 资源（而非字体 emoji）；Image::Tint
// 令其随深浅主题的状态栏次级文字色变化，24×24 画布经 Contain 收进 14pt 图标槽。
huxerui::View StatusActionImage(const huxerui::ImageResource& icon, std::string label,
                                std::string tooltip, const huxerui::ThemeSpec& theme,
                                std::function<void()> onClick) {
    const huxerui::TextStyle style{
        .font = huxerui::Font::System(font_size::kCaption),
        .foreground = theme.colors.on_surface_variant};
    return huxerui::Row {
        huxerui::Image(icon)
            .Fit(huxerui::ImageFit::Contain)
            .Align(huxerui::HorizontalAlignment::Center,
                   huxerui::VerticalAlignment::Center)
            .Tint(theme.colors.on_surface_variant)
            .With(huxerui::Frame{.width = 14.0F, .height = 14.0F}),
        huxerui::Text(std::move(label), huxerui::TextRole::Label).Style(style),
    }
        .With(huxerui::Spacing(4.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::Padding(huxerui::EdgeInsets::Symmetric(theme.spacing.small, 2.0F)),
              huxerui::Tooltip(tooltip),
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button, .label = tooltip})
        .OnClick(std::move(onClick));
}

// 请求代理弹窗：单行 Outlined TextField（初值 = 当前 request_proxy）+ 说明 +
// 取消/保存。保存 = saveSessionPreference("request_proxy")（存储键与 store 契约
// 一致：finalizeSpec/globalProxy() 读该键并自行 trim）+ toast + 关弹窗；
// bump version 让状态条摘要文本随重组刷新。写 KV/State 均不卸载子树，同步安全
// （约定 6 只约束会导致点击节点卸载的写）。
[[huxerui::composable]] huxerui::View RequestProxyDialogContent(
    huxerui::DialogContext ctx, huxerui::State<int> version) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    (void)version.Get(); // 订阅：弹窗存续期内外部 bump 时重组（text 缓冲不被初值重置）。
    std::string initial = trim(sessionPreference("request_proxy"));
    auto text = huxerui::UseState(huxerui::TextEditingValue{std::move(initial)});
    return DialogCard(huxerui::Column {
        huxerui::Text("请求代理", huxerui::TextRole::Title),
        huxerui::TextField(text)
            .Label("代理地址")
            .Placeholder("http://host:port 或 socks5://host:port")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([text](const huxerui::TextEditingValue& value) { text = value; }),
        huxerui::Text(
            "支持 http://host:port、socks5://host:port；作用于所有单次 HTTP 请求（curl "
            "引擎），k6 压测不受影响；留空 = 直连。",
            huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("保存").OnClick([ctx, text, version, toast] {
                const std::string proxy = trim(text.Get().text);
                saveSessionPreference("request_proxy", proxy);
                toast.Show(proxy.empty() ? "已清除请求代理（直连）" : "请求代理已保存");
                ctx.Dismiss();
                version = version.Get() + 1;
            }),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 380.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 项目 Cookie 管理弹窗（当前项目）：文本表使用 Params 同源的 KvTable。
// 编辑模型（选型说明）：行编辑态存 dialog 层 State<std::vector<CookieRow>> 缓冲，
// 行内保留完整 TextEditingValue——若逐键直通写库（OnChanged 即 saveGlobalCookie），
// 每字符一次 SQLite upsert、失败时 toast 刷屏，且重组回读 store 只剩纯文本会丢
// 光标。改为：文本改动只进缓冲，「保存」按钮统一落库（小表整表 upsert，不 diff；
// 名称非空才物化，空名行丢弃）；Checkbox 启用/禁用与 ✕ 删除是离散操作即点即落库
// （✕ 所在行会卸载，KvTable 内部负责推迟出指针事件路径，约定 6）。
// 编辑表使用共享 KvTable（同为缓冲 + 统一保存 + 虚拟末行）。
[[huxerui::composable]] huxerui::View GlobalCookieDialogContent(
    huxerui::DialogContext ctx, huxerui::State<int> version) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    (void)version.Get();
    std::vector<CookieRow> initial = CookieRowsFromStore();
    auto rows = huxerui::UseState(std::move(initial));

    return DialogCard(huxerui::Column {
        huxerui::Text("项目 Cookie", huxerui::TextRole::Title),
        huxerui::Text("启用的项目 Cookie 在每次发送时并入请求；项目级静态值，不参与 "
                      "{{var}} 环境变量替换。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        KvTable(CookieKvRows(rows.Get()), theme, "Cookie 名", "Cookie 值",
                [rows](std::vector<KvRow> values) {
                    rows = ReconcileCookieRows(rows.Get(), std::move(values));
                },
                KvTableOptions{.show_type = false, .show_remark = false})
            .With(huxerui::Frame{.max_height = 300.0F}),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("保存").OnClick([ctx, rows, toast, version] {
                // 先 upsert 有效行；全部成功后再删去未保留的旧行，避免写入中途
                // 失败时丢失 Cookie。id 仅用于更新/删除，不影响 Cookie 语义。
                const std::vector<db::GlobalCookie> previous = g_requests.globalCookies();
                std::vector<CookieRow> kept;
                bool failed = false;
                for (CookieRow r : rows.Get()) {
                    if (r.name.text.empty()) continue;
                    db::GlobalCookie cookie{r.id, 0, r.name.text, r.value.text, r.enabled};
                    if (const std::string err = g_requests.saveGlobalCookie(cookie);
                        !err.empty()) {
                        toast.Show("保存 Cookie 失败: " + err);
                        failed = true;
                        kept.push_back(std::move(r));
                        continue;
                    }
                    r.id = cookie.id;
                    kept.push_back(std::move(r));
                }
                if (failed) {
                    rows = std::move(kept);
                    return;
                }
                for (const db::GlobalCookie& cookie : previous) {
                    bool retained = false;
                    for (const CookieRow& row : kept) {
                        if (row.id == cookie.id) retained = true;
                    }
                    if (!retained) {
                        if (const std::string err = g_requests.deleteGlobalCookie(cookie.id);
                            !err.empty()) {
                            toast.Show("删除 Cookie 失败: " + err);
                            rows = std::move(kept);
                            return;
                        }
                    }
                }
                rows = std::move(kept);
                version = version.Get() + 1;
                ctx.Dismiss();
            }),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 520.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 底部项目状态条：仅项目工作区挂载，与侧边栏一样无岛屿包裹——无背景、无顶部分隔线；
// 主页和通用设置由其大岛直接覆盖剩余高度。右缘热区管理应用代理 / 项目 Cookie。
// 独立成 provider 之下的 composable：弹窗层捕获调用处环境（CLAUDE.md），AppRoot
// 自身在 OceanThemed provider 之上、UseTheme/UseDialog 只能拿默认浅色——热区
// 弹窗必须从这里 Show 才带正确主题（同 CloseGuard 的根因与做法）。
} // namespace

[[huxerui::composable]] huxerui::View GlobalStatusBar(float statusTopPad) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    // 重组触发器：代理/Cookie 弹窗落库后 bump，本作用域重读 sessionPreference，
    // 右缘代理摘要随之下一次组合刷新。
    auto version = huxerui::UseState(0);
    (void)version.Get();
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    // 数据：以领域 store 为权威读取（组合期快照）。刷新时机：切页/切项目外壳重组、
    // 本组件随重组刷新；store 内部变化（环境切换、k6 探测结果翻转）不触发外壳重组，
    // 显示值待下一次自然重组更新（值低频变化，不为此新造订阅机制）。
    std::string statusProject = "未打开项目";
    for (const db::Project& p : g_requests.projects()) {
        if (p.id == g_requests.currentProjectId()) {
            statusProject = p.name;
            break;
        }
    }
    std::string statusEnv = "无环境";
    if (const db::Environment* env = g_requests.findEnvironment(g_requests.currentEnvId())) {
        statusEnv = env->name;
    }
    // curl 引擎恒就绪（store 构造即持有）；k6 按引擎探测结果显示。
    const std::string statusK6 = g_loadtest.available() ? "k6: 就绪" : "k6: 未找到";
    const std::string proxy = trim(sessionPreference("request_proxy"));
    const std::string proxyLabel =
        "应用代理: " + (proxy.empty() ? std::string("无") : TruncateSummary(proxy, 24));
    const auto statusText = [&theme](std::string text) {
        return huxerui::Text(std::move(text), huxerui::TextRole::Label)
            .Style(huxerui::TextStyle{
                .font = huxerui::Font::System(font_size::kCaption),
                .foreground = theme.colors.on_surface_variant});
    };

    std::vector<huxerui::View> items;
    items.push_back(statusText(statusProject));
    if (!compact) {
        items.push_back(statusText(statusEnv));
        items.push_back(statusText("HTTP: curl"));
        items.push_back(statusText(statusK6));
    }
    items.push_back(
        // 右缘占位：空 Row + Grow(1.0F) 吃掉剩余宽度，把两个热区推到行尾。
        // 约定 8：零尺寸占位不用 Spacer（自带 Grow(1) 的隐式语义留给真弹性项）。
        huxerui::Row{}.With(huxerui::Grow(1.0F)));
    items.push_back(StatusActionText("⇄", compact ? "代理" : proxyLabel,
                         "设置应用级代理（curl 引擎，k6 压测不受影响）", theme,
                         [dialog, version] {
                             // 开弹窗是层操作、不卸载按钮子树，指针事件路径上同步 Show
                             // 安全（先例：树行右键直接 ShowPopupMenuAt）。
                             dialog.Show(
                                 [version](huxerui::DialogContext ctx) mutable -> huxerui::View {
                                     return RequestProxyDialogContent(ctx, version);
                                 },
                                 huxerui::DialogOptions{});
                         }));
    items.push_back(StatusActionImage(app::images::cookie, compact ? "Cookie" : "项目 Cookie",
                         "管理当前项目的项目 Cookie", theme,
                         [dialog, version] {
                             dialog.Show(
                                 [version](huxerui::DialogContext ctx) mutable -> huxerui::View {
                                     return GlobalCookieDialogContent(ctx, version);
                                 },
                                 huxerui::DialogOptions{});
                         }));
    return huxerui::Row(std::move(items))
        .With(huxerui::Spacing(compact ? theme.spacing.small : theme.spacing.medium),
              // 左右留白与侧栏/页面边距对齐（spacing.medium）；顶部补偿 padding 补回
              // 根 Column 收窄的间隙：主行↔状态条保持 gap 不变。
              huxerui::Padding(huxerui::EdgeInsets{.top = statusTopPad,
                                                   .right = theme.spacing.medium,
                                                   .bottom = 0.0F,
                                                   .left = theme.spacing.medium}),
              huxerui::Frame{.height = 22.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center));
}


} // namespace apitab::ui
