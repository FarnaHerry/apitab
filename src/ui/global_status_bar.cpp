// global_status_bar.cpp — 底部全局状态条（P1-C2 自 app.cpp 纯搬移）：
//   TruncateSummary / CookieRow / CookieRowsFromStore / StatusActionText /
//   RequestProxyDialogContent / GlobalCookieDialogContent / GlobalStatusBar。
//   右缘两个文字热区（应用代理 / 项目 Cookie）点击开弹窗；弹窗层捕获调用处环境
//   （AppRoot 在 MinimalThemed 之上，UseTheme 须在 provider 之下，故 GlobalStatusBar
//   独立成 composable）。P1-B1 前保持现状，不拆为项目/状态两栏。
#include <huxerui/huxerui.h>

#include <functional>
#include <string>
#include <vector>

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

// 项目 Cookie 的行编辑缓冲元素：store 行（id/enabled）+ 完整 TextEditingValue
// （保留光标与选区，逐键回写不重置插入点）。
struct CookieRow {
    std::int64_t id = 0;
    huxerui::TextEditingValue name;
    huxerui::TextEditingValue value;
    bool enabled = true;

    // State<vector<CookieRow>> 写入去重需要值比较（TextEditingValue 自带 ==，
    // 同 draft.h KvRow）。
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

// 状态条右缘文字热区：kCaption 小字 + Padding 热区 + Tooltip + 点击开弹窗。
huxerui::View StatusActionText(std::string label, std::string tooltip,
                               const huxerui::ThemeSpec& theme,
                               std::function<void()> onClick) {
    return huxerui::Text(std::move(label), huxerui::TextRole::Label)
        .Style(huxerui::TextStyle{.font = huxerui::Font::System(font_size::kCaption),
                                  .foreground = theme.colors.on_surface_variant})
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(theme.spacing.small, 2.0F)),
              huxerui::Tooltip(std::move(tooltip)))
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

// 项目 Cookie 管理弹窗（当前项目）。
// 编辑模型（选型说明）：行编辑态存 dialog 层 State<std::vector<CookieRow>> 缓冲，
// 行内保留完整 TextEditingValue——若逐键直通写库（OnChanged 即 saveGlobalCookie），
// 每字符一次 SQLite upsert、失败时 toast 刷屏，且重组回读 store 只剩纯文本会丢
// 光标。改为：文本改动只进缓冲，「保存」按钮统一落库（小表整表 upsert，不 diff；
// 名称非空才物化，空名行丢弃）；Checkbox 启用/禁用与 ✕ 删除是离散操作即点即落库
// （✕ 所在行会卸载，经 tasks.Launch + Delay(0) 推迟出指针事件路径，约定 6）。
// 先例：project_settings_page.cpp ProjectHeaderTable（同为缓冲 + 统一保存 + 虚拟末行）。
[[huxerui::composable]] huxerui::View GlobalCookieDialogContent(
    huxerui::DialogContext ctx, huxerui::State<int> version) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    (void)version.Get(); // 订阅：写库/删除 bump 后重组本弹窗（rows 缓冲保留，光标不重置）。
    std::vector<CookieRow> initial = CookieRowsFromStore(); // 初值在 UseState 之前算好（store 权威）。
    auto rows = huxerui::UseState(std::move(initial));

    const std::vector<CookieRow> data = rows.Get();
    std::vector<huxerui::View> children{
        huxerui::Row {
            huxerui::Text("", huxerui::TextRole::Label).With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text("名称", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
            huxerui::Text("值", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Foreground(theme.colors.on_surface_variant)),
    };
    for (std::size_t i = 0; i <= data.size(); ++i) {
        const bool phantom = i == data.size();
        const CookieRow row = phantom ? CookieRow{} : data[i];
        // 行写入缓冲：越界（虚拟行）时仅名称非空才物化追加。
        auto applyRow = [rows](std::size_t i, CookieRow updated) {
            std::vector<CookieRow> copy = rows.Get();
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.name.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            rows = copy;
        };
        // 离散操作即落库：先写缓冲再 upsert；新物化行回填 id。
        auto saveRow = [rows, toast, applyRow](std::size_t i, const CookieRow& updated) {
            applyRow(i, updated);
            if (updated.name.text.empty()) return;
            db::GlobalCookie cookie{updated.id, 0, updated.name.text, updated.value.text,
                                    updated.enabled};
            if (const std::string err = g_requests.saveGlobalCookie(cookie); !err.empty()) {
                toast.Show("保存 Cookie 失败: " + err);
                return;
            }
            if (cookie.id != updated.id) {
                std::vector<CookieRow> copy = rows.Get();
                if (i < copy.size()) {
                    copy[i].id = cookie.id;
                    rows = copy;
                }
            }
        };
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(row.enabled).OnChanged([row, i, saveRow](bool checked) {
                    CookieRow updated = row;
                    updated.enabled = checked;
                    saveRow(i, updated);
                }),
                huxerui::TextField(row.name)
                    .Label("名称")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        CookieRow updated = row;
                        updated.name = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(row.value)
                    .Label("值")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        CookieRow updated = row;
                        updated.value = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                phantom
                    ? huxerui::View{huxerui::Row{}.With(
                          huxerui::Frame{.width = 28.0F, .height = 28.0F})}
                    : AppIconButton("✕", "删除 Cookie",
                          [tasks, rows, toast, version, row, i] {
                              // 删除会卸载 ✕ 所在行：推迟出指针事件路径（约定 6）；
                              // 落库删除同样在推迟任务里做，失败则保留缓冲行。
                              tasks.Launch([=]() -> huxerui::Task<void> {
                                  co_await huxerui::Delay(std::chrono::duration<double>{0});
                                  if (row.id != 0) {
                                      if (const std::string err =
                                              g_requests.deleteGlobalCookie(row.id);
                                          !err.empty()) {
                                          toast.Show("删除 Cookie 失败: " + err);
                                          co_return;
                                      }
                                  }
                                  std::vector<CookieRow> copy = rows.Get();
                                  if (i < copy.size()) {
                                      copy.erase(copy.begin() + static_cast<long>(i));
                                  }
                                  rows = copy;
                                  version = version.Get() + 1;
                              });
                          }, AppIconButtonShape::Bare),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }

    return DialogCard(huxerui::Column {
        huxerui::Text("项目 Cookie", huxerui::TextRole::Title),
        huxerui::Text("启用的项目 Cookie 在每次发送时并入请求；项目级静态值，不参与 "
                      "{{var}} 环境变量替换。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::ScrollView{huxerui::Column(std::move(children))
                                .With(huxerui::Spacing(theme.spacing.small),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar{}, huxerui::Frame{.max_height = 300.0F}),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::Button("保存").OnClick([ctx, rows, toast, version] {
                // 缓冲统一落库：空名行不物化直接丢；有行失败则保持弹窗打开，
                // 成功行回填 id（不重复写）。
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
                version = version.Get() + 1;
                if (failed) {
                    rows = std::move(kept);
                } else {
                    ctx.Dismiss();
                }
            }),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 520.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

// 底部全局状态条（所有页面共享）：与侧边栏一样无岛屿包裹——无背景、无顶部分隔线
// （分隔线已删：根底色与上方岛屿已有层次，线条多余），一行小字直接落在窗口背景上，
// 右缘两个文字热区（请求代理 / 全局 Cookie）点击开弹窗。
// 独立成 provider 之下的 composable：弹窗层捕获调用处环境（CLAUDE.md），AppRoot
// 自身在 MinimalThemed provider 之上、UseTheme/UseDialog 只能拿默认浅色——热区
// 弹窗必须从这里 Show 才带正确主题（同 CloseGuard 的根因与做法）。
} // namespace

[[huxerui::composable]] huxerui::View GlobalStatusBar(float statusTopPad) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto dialog = huxerui::UseDialog();
    // 重组触发器：代理/Cookie 弹窗落库后 bump，本作用域重读 sessionPreference，
    // 右缘代理摘要随之下一次组合刷新。
    auto version = huxerui::UseState(0);
    (void)version.Get();

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

    return huxerui::Row {
        statusText(statusProject),
        statusText(statusEnv),
        statusText("HTTP: curl"),
        statusText(statusK6),
        // 右缘占位：空 Row + Grow(1.0F) 吃掉剩余宽度，把两个热区推到行尾。
        // 约定 8：零尺寸占位不用 Spacer（自带 Grow(1) 的隐式语义留给真弹性项）。
        huxerui::Row{}.With(huxerui::Grow(1.0F)),
        StatusActionText(proxyLabel, "设置应用级代理（curl 引擎，k6 压测不受影响）",
                         theme,
                         [dialog, version] {
                             // 开弹窗是层操作、不卸载按钮子树，指针事件路径上同步 Show
                             // 安全（先例：树行右键直接 ShowPopupMenuAt）。
                             dialog.Show(
                                 [version](huxerui::DialogContext ctx) mutable -> huxerui::View {
                                     return RequestProxyDialogContent(ctx, version);
                                 },
                                 huxerui::DialogOptions{});
                         }),
        StatusActionText("项目 Cookie", "管理当前项目的项目 Cookie", theme,
                         [dialog, version] {
                             dialog.Show(
                                 [version](huxerui::DialogContext ctx) mutable -> huxerui::View {
                                     return GlobalCookieDialogContent(ctx, version);
                                 },
                                 huxerui::DialogOptions{});
                         }),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
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
