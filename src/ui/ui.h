// ui.h — HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app codegen）。
#pragma once

#include <huxerui/huxerui.h>
#include <sweetedit_core/sweet_editor.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace apitab::ui {

// SlideCell / StartSlide：标签实时换位（拖过即交换）时邻居的让位滑动，
// FLIP 简化版——数据重排后布局立即落到新槽位，给被挤动的标签加一个等于
// 位移量的反向 Offset 残量（视觉留在原位），再用手写 tween（16ms 步进、
// ease-out 立方）衰减到 0，即看到它从旧槽位滑到新槽位。同一标签连滑时
// 残量累加（视觉连续），代数守卫作废旧 tween。
// 为什么不用 SDK 的 Offset(Animated<Point>)：那个模型要"先 Set 残量、
// 再 Animated 归零"两次重组相位，与实时换位的单步数据变更对不齐。
struct SlideCell {
    std::unordered_map<std::int64_t, float> offsets; // 当前残量（px）
    std::unordered_map<std::int64_t, std::uint64_t> gens; // 每标签 tween 代数
};

inline float SlideOffsetOf(const std::shared_ptr<SlideCell>& cell, std::int64_t key) {
    const auto it = cell->offsets.find(key);
    return it == cell->offsets.end() ? 0.0F : it->second;
}

// deltaX：该标签本次槽位变化的反向残量（左移一格 = +stride）。tick 是纯
// 重组触发器（组合体须 (void)tick.Get() 订阅）。
inline void StartSlide(huxerui::TaskScope tasks,
                       huxerui::State<std::shared_ptr<SlideCell>> cell,
                       huxerui::State<std::uint64_t> tick, std::int64_t key, float deltaX) {
    if (deltaX == 0.0F) return;
    auto c = cell.Get();
    c->offsets[key] += deltaX;
    const float start = c->offsets[key];
    const std::uint64_t gen = ++c->gens[key];
    tick = tick.Get() + 1; // 立即重组一次，应用起始残量
    tasks.Launch([cell, tick, key, gen, start]() -> huxerui::Task<void> {
        constexpr int kSteps = 8; // 8×16ms ≈ 0.13s
        for (int s = 1; s <= kSteps; ++s) {
            co_await huxerui::Delay(std::chrono::duration<double>{0.016});
            auto c2 = cell.Get();
            const auto it = c2->gens.find(key);
            if (it == c2->gens.end() || it->second != gen) co_return; // 被更新的滑动取代
            const float p = static_cast<float>(s) / static_cast<float>(kSteps);
            const float inv = 1.0F - p;
            c2->offsets[key] = start * inv * inv * inv; // ease-out 立方衰减到 0
            tick = tick.Get() + 1;
        }
        auto c3 = cell.Get();
        if (c3->gens[key] == gen) c3->offsets.erase(key); // 收尾防 map 膨胀
    });
}

// 全项目统一字号阶梯（pt）：控件/正文跟随 SDK 默认 14，不再散落硬编码字面量；
// 调整字号只改这一处。等宽字号用于响应内容与 HTTP 方法/类型徽标。
namespace font_size {
inline constexpr float kCaption = 11.0F;  // 行内操作符（✎/✕）、徽标等小字
inline constexpr float kChip = 12.0F;     // 标签条等紧凑部件文字
inline constexpr float kBody = 14.0F;     // 正文/按钮/输入框（SDK 默认）
inline constexpr float kMonoBody = 13.0F; // 等宽正文（响应 Body/Headers/Cookies）
inline constexpr float kTitle = 20.0F;    // 标题（弹窗标题等）
} // namespace font_size

// HTTP 方法统一配色（全项目唯一色表，深浅主题各一套）：GET 绿 / POST 琥珀 /
// PUT 蓝 / PATCH 紫 / DELETE 玫红（**不是** error 红）/ HEAD 灰蓝 /
// OPTIONS 青 / CONNECT 棕 / TRACE 灰 / QUERY 靛蓝 / PURGE 深橙。
// 注意 **危险色（theme.colors.error）独立出来，专属删除按钮/危险菜单项/确认
// 弹窗等危险操作**，方法 UI 一律走本表。WebDAV 长尾（PROPFIND 等）与
// 非 HTTP 徽标（WS/TCP）回落中性色 on_surface_variant。
inline huxerui::Color MethodColor(const huxerui::ThemeSpec& theme, std::string_view method) {
    struct Swatch {
        std::string_view name;
        huxerui::Color light;
        huxerui::Color dark;
    };
    static constexpr Swatch kSwatches[] = {
        {"GET", huxerui::Color::Rgb(30, 125, 50), huxerui::Color::Rgb(107, 203, 119)},
        {"POST", huxerui::Color::Rgb(178, 106, 0), huxerui::Color::Rgb(229, 192, 123)},
        {"PUT", huxerui::Color::Rgb(21, 101, 192), huxerui::Color::Rgb(97, 175, 239)},
        {"PATCH", huxerui::Color::Rgb(106, 63, 181), huxerui::Color::Rgb(198, 120, 221)},
        {"DELETE", huxerui::Color::Rgb(194, 24, 91), huxerui::Color::Rgb(240, 98, 146)},
        {"HEAD", huxerui::Color::Rgb(84, 110, 122), huxerui::Color::Rgb(144, 164, 174)},
        {"OPTIONS", huxerui::Color::Rgb(0, 131, 143), huxerui::Color::Rgb(77, 182, 172)},
        {"CONNECT", huxerui::Color::Rgb(93, 64, 55), huxerui::Color::Rgb(188, 170, 164)},
        {"TRACE", huxerui::Color::Rgb(117, 117, 117), huxerui::Color::Rgb(189, 189, 189)},
        {"QUERY", huxerui::Color::Rgb(63, 81, 181), huxerui::Color::Rgb(121, 134, 203)},
        {"PURGE", huxerui::Color::Rgb(230, 74, 25), huxerui::Color::Rgb(255, 138, 101)},
    };
    const bool light = theme.colors.surface.red > 0.5F;
    for (const Swatch& s : kSwatches)
        if (s.name == method) return light ? s.light : s.dark;
    return theme.colors.on_surface_variant;
}

// common.cpp
huxerui::View PageHeader(std::string title, std::string subtitle);
huxerui::View MigrationPlaceholder(std::string pageName);
huxerui::View EmptyState(huxerui::ImageResource icon, std::string title, std::string subtitle,
                         huxerui::State<std::size_t> navPage);
// 纯圆形单符号按钮（如 "+"）：无文字标签。accent=true 主色底（新建等主动作）；
// accent=false 中性容器底（溢出菜单等次动作）。
huxerui::View CircleButton(std::string glyph, std::function<void()> onClick, bool accent = true);
// 自定义内容弹窗的卡片包裹：SDK 的 dialog.Show(ViewFactory/DialogFactory) 不给
// 内容加底板（只有标题+消息的内置形态才有 DialogStyle），裸内容直接浮在页面上
// 分不清层级。统一包一层：圆角底 + 阴影 + 内边距。
huxerui::View DialogCard(huxerui::View content);
// 破坏性操作（删除/清空等）的确认弹窗：内置 dialog.Show(title, message, ...) 的
// DialogStyle 是 Environment 全局值，无法按调用点给确认按钮换色，故用 DialogCard
// + 自定义按钮行实现，确认按钮染主题 error 红。点确认先关弹窗再调 onConfirm；
// onConfirm 内若会卸载被点节点，沿用 tasks.Launch + Delay(0) 推迟惯例。
void ShowDangerConfirm(huxerui::DialogHandle dialog, std::string title, std::string message,
                       std::string confirmLabel, std::function<void()> onConfirm);
// 自绘弹出菜单（UsePopup 承载）。作者口径：通用 MenuItem 不支持 per-item 配色/
// hover 定制，需要就自己用 UsePopup 做菜单内容——本组件即该配方。条目语义对齐
// MenuItem：label + 点击回调 + 可选选中态 + 危险模式；外观对齐环境 MenuStyle，
// 条目悬停/按压复用 item_indication。**选中项不用对钩，填充比 hover 深一档的
// 底色**（取 item_indication.press 填充色）。点击先 Dismiss 再回调（同系统菜单）。
enum class PopupMenuDanger {
    kNone,     // 常规项
    kHoverRed, // 危险项：常态普通色，hover 时文字变 error 红
    kAlwaysRed // 危险项：常驻 error 红（如方法下拉的 DELETE）
};
struct PopupMenuItem {
    std::string label;
    std::function<void()> on_click;
    bool checked = false;
    PopupMenuDanger danger = PopupMenuDanger::kNone;
    // 自定义文字色（如方法下拉按 MethodColor 逐方法着色）；设置后优先于
    // danger 取色。
    std::optional<huxerui::Color> label_color;
};
void ShowPopupMenu(huxerui::PopupHandle popup, std::vector<PopupMenuItem> items,
                   const huxerui::PopupOptions& options = {});
void ShowPopupMenuAt(huxerui::PopupHandle popup, huxerui::Point point,
                     std::vector<PopupMenuItem> items,
                     const huxerui::PopupOptions& options = {});
// SweetEditor 调色板（apitab 本地补丁新增的 SweetEditorOptions::palette）：默认构造
// = 上游浅色主题。浅色主题直接返回默认；深色主题返回一套以 apitab 深色令牌为骨架
// （结构色取自 ThemeSpec，语法/补全强调色取 VS Code Dark+ 近似值）的配色。
sweetedit_huxer::SweetEditorPalette EditorPalette(const huxerui::ThemeSpec& theme);
// 方法 + URL 合并控件（Postman 风格）：左侧扁平方法选择（文本 ▾ 弹菜单），
// 之后是当前环境 baseUrl 显示区（灰色只读、可截断；为空则不渲染；URL 输入自带
// URI scheme 时弱化显示表示不拼接），1pt 分隔线，右侧 URL 输入，整体共用一个
// 描边圆角外框。自带 Grow(1)。
huxerui::View MethodUrlBar(std::vector<std::string> methods, std::size_t methodIndex,
                           std::function<void(std::size_t)> onMethodChanged,
                           huxerui::TextEditingValue url,
                           std::function<void(const huxerui::TextEditingValue&)> onUrlChanged,
                           std::string baseUrl, std::string placeholder);

// home_page.cpp
huxerui::View HomePage(huxerui::State<std::size_t> navPage,
                       huxerui::State<std::vector<std::int64_t>> tabs,
                       huxerui::State<std::int64_t> activeProject);

// request_page.cpp
huxerui::View RequestPage(huxerui::State<std::int64_t> activeProject);

// loadtest_page.cpp
huxerui::View LoadTestPage();

// websocket_page.cpp
huxerui::View WebSocketPage();

// tcp_page.cpp
huxerui::View TcpPage();

// history_page.cpp
huxerui::View HistoryPage();

// settings_page.cpp（全局设置：主题模式 + 关闭行为，状态由 AppRoot 持有）
huxerui::View GlobalSettingsPage(huxerui::State<int> themeMode, huxerui::State<int> closeBehavior);

// project_settings_page.cpp（当前项目设置）
huxerui::View ProjectSettingsPage();

// http_test_page.cpp（框架自带 HTTP + 协程的并发压测实验页）
huxerui::View HttpTestPage();

} // namespace apitab::ui
