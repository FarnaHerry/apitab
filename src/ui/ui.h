// ui.h — HuxerUI 前端内部声明（UI 层是普通 C++ 源，经 huxerui_add_app codegen）。
#pragma once

#include <huxerui/huxerui.h>
#include <sweetedit_core/sweet_editor.h>

#include "draft.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
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
// 图标按钮字形固定字号（官方建议 14–16pt，取 15）：glyph 只负责绘制，字号不再
// 反向决定按钮命中区（见 AppIconButton / 几何令牌）。
inline constexpr float kIconGlyph = 15.0F;
} // namespace font_size

// 岛屿结构的语义层级。页面通过语义层级选择表面，不直接依赖 Material 的
// surface_container_* 命名；颜色仍由当前 ThemeSpec 派生，深浅主题共用组件。
enum class IslandLevel {
    Base,
    Raised,
    Active,
    Overlay,
    Danger,
};

struct IslandTheme {
    float page_gap;
    float island_padding;
    float island_radius;
    float nested_radius;
    // ---- 几何令牌（P1-A1，全项目唯一圆角/控件尺寸来源）----
    // control_radius：普通按钮/选择器/局部分组的 8pt 圆角（取 theme.shapes.small）；
    // large_control_radius：大输入行/请求组合栏的 12pt 大圆角（取 theme.shapes.medium）；
    // icon_button_compact / icon_button_regular：图标按钮正方形命中区两档 28/32pt；
    // control_height：普通控件（按钮/输入行）统一高度 32pt。
    // island_radius/nested_radius 维持一级岛 16pt / 二级岛与浮动菜单 8pt。
    // **页面禁止再散落圆角/图标按钮尺寸魔法数字**——同类列表、组合栏、按钮的
    // 几何只从这里取值（经 ResolveIslandTheme），调整尺寸只改这一处。
    float control_radius;
    float large_control_radius;
    float icon_button_compact;
    float icon_button_regular;
    float control_height;
    float rail_width;
    huxerui::Color ocean;
    huxerui::Color base;
    huxerui::Color raised;
    huxerui::Color active;
    huxerui::Color overlay;
    huxerui::Color outline_soft;
};

IslandTheme ResolveIslandTheme(const huxerui::ThemeSpec& theme);

// 按父/子表面的圆角与 inset 解析内层同心圆角：inner = outer − inset，
// 下限 4pt（小于该值时内外圆角几乎相切，视觉上出现反同心）。供嵌套表面
// （列表行、组合栏内部控件等）迁移使用，页面不自行计算。
float ConcentricRadius(float outer_radius, float inset);

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

// ---- 顶级标签状态模型（island-structure-theme.md §13.1/§13.2，P1-B0.1）----
// 顶层位置不再由 activeProject + navPage 两个可互相矛盾的状态共同表达（旧模型
// 根因：进设置只改 navPage、项目仍 active，再点同一项目时 activateProject 只在
// navPage==kHome 时切回请求页 → 卡在设置页），统一为同一套顶级标签行为：
//   - 主页标签：固定最左，不可关闭、不参与拖拽；
//   - 项目标签：可打开/关闭/拖拽换位/持久化（open_projects）；
//   - 全局设置标签：单例（未开则开、已开仅激活），不参与拖拽与 open_projects 持久化。
// navPage 从此只表达项目工作区内部页（kRequest/kLoad/kHistory/kProjectSettings
// + 遗留 kHttpTest），不再包含 kHome/kAppSettings 等顶级目的地。
enum class TopTabKind {
    Home,
    Project,
    GlobalSettings,
};

struct TopTabId {
    TopTabKind kind = TopTabKind::Home;
    std::int64_t project_id = 0; // 仅 Project 使用
    bool operator==(const TopTabId&) const = default;
};

// 顶级标签状态的纯数据镜像，与 AppRoot 的 State 一一对应（open_projects ↔ tabs、
// settings_open ↔ settingsOpen、active ↔ activeTopTab、last_project ↔ lastProjectTab）。
// 纯 helper 不依赖任何 HuxerUI 类型：AppRoot 在推迟任务里取快照 → 调 helper →
// 写回 State，也便于脱离 UI 覆盖打开/重复打开/切换/关闭回退矩阵（§13.4 B0.1 的
// 「用纯状态函数覆盖回退矩阵」要求）。
struct TopTabState {
    std::vector<std::int64_t> open_projects; // 顺序即标签顺序（= 持久化 open_projects CSV 格式）
    bool settings_open = false;              // 设置单例标签是否存在（不持久化）
    TopTabId active;                         // active_top_tab
    std::int64_t last_project = 0;           // last_project_tab：最近激活且仍打开的项目（0 = 无，不持久化）
};

// 以下 helper 全部为纯函数：不改入参、不碰 State/领域 store。领域写入
// （selectProject/setProject/saveSessionPreference）由 AppRoot 的推迟任务在调用
// helper 之后完成（§13.2 不变量 1：先领域写入再写 activeTopTab，同一任务内完成）。

// 主页标签：固定最左、不可关闭。激活只改 active——领域当前项目与打开列表不动
//（返回项目时避免重新加载，§13.1；视觉 active 由 activeTopTab 决定）。
inline TopTabState ActivateHome(TopTabState s) {
    s.active = TopTabId{};
    return s;
}

// 项目标签：不在 open_projects 则追加（打开）；active=Project(id)；last_project=id；
// settings_open 保持——设置标签留在后台，可再次激活（单例，§13.1）。
// 重复激活同一项目 = 无条件切回顶级标签（§13.2 项 3），顺序不变。
inline TopTabState ActivateProject(TopTabState s, std::int64_t project_id) {
    if (std::ranges::find(s.open_projects, project_id) == s.open_projects.end()) {
        s.open_projects.push_back(project_id);
    }
    s.active = TopTabId{TopTabKind::Project, project_id};
    s.last_project = project_id;
    return s;
}

// 设置单例标签：settings_open=true；active=GlobalSettings（已打开再点 = 仅激活，
// 不新增标签）。open_projects/last_project 不动；不持久化。
inline TopTabState ActivateSettings(TopTabState s) {
    s.settings_open = true;
    s.active = TopTabId{TopTabKind::GlobalSettings, 0};
    return s;
}

// 关闭顶级标签。回退矩阵（§13.2 不变量 4/5）：
// - 关项目（不管是否活动）：从 open_projects 删除。若是活动项目：回退 = 右邻优先、
//   无右邻取左邻、仍无则 Home；last_project 若指向被关项目则改为新活动项目 id
//   （新活动非项目则 0）。关非活动项目：不改 active/last_project。
// - 关设置：settings_open=false。若设置当时是活动标签：回退 = last_project 若仍在
//   open_projects，否则取 open_projects 最后一项，仍无则 Home；last_project 随
//   活动项目同步（回主页则 0）。设置单例只有一个、若活动必是它——「关非活动
//   设置」按防御处理（仅置 settings_open=false）。
// - 关 Home：主页标签不可关闭，防御性 no-op；target 不在打开列表：防御性 no-op。
inline TopTabState CloseTopTab(TopTabState s, const TopTabId& target) {
    switch (target.kind) {
        case TopTabKind::Home:
            return s;
        case TopTabKind::Project: {
            const auto it = std::ranges::find(s.open_projects, target.project_id);
            if (it == s.open_projects.end()) return s;
            const auto index = static_cast<std::size_t>(
                std::distance(s.open_projects.begin(), it));
            s.open_projects.erase(it);
            if (!(s.active == target)) return s; // 关非活动项目：active/last_project 不变
            // 回退：右邻（删除后落回原下标的元素）优先 → 左邻 → Home。
            std::int64_t fallback = 0;
            if (index < s.open_projects.size()) {
                fallback = s.open_projects[index];
            } else if (index > 0) {
                fallback = s.open_projects[index - 1];
            }
            s.active = fallback != 0 ? TopTabId{TopTabKind::Project, fallback} : TopTabId{};
            if (s.last_project == target.project_id) s.last_project = fallback;
            return s;
        }
        case TopTabKind::GlobalSettings: {
            s.settings_open = false;
            if (s.active.kind != TopTabKind::GlobalSettings) return s; // 防御（见上）
            // 回退：last_project 仍在打开列表 → 激活它；否则取最后一项；仍无 → 主页。
            std::int64_t fallback = 0;
            if (s.last_project != 0 &&
                std::ranges::find(s.open_projects, s.last_project) != s.open_projects.end()) {
                fallback = s.last_project;
            } else if (!s.open_projects.empty()) {
                fallback = s.open_projects.back();
            }
            s.active = fallback != 0 ? TopTabId{TopTabKind::Project, fallback} : TopTabId{};
            s.last_project = fallback; // 随活动项目同步（回主页则 0）
            return s;
        }
    }
    return s;
}

// P1-B0.5 启动恢复：解析 open_projects CSV（逗号分隔，容忍空格/空项/非法数字），
// 按出现顺序去重（首现保留），返回去重后的 id 列表（不做存在性过滤，过滤在 RestoreTopTabs）。
inline std::vector<std::int64_t> ParseOpenProjectsCsv(const std::string& csv) {
    std::vector<std::int64_t> out;
    std::unordered_map<std::int64_t, bool> seen;
    std::string_view s(csv);
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(',', start);
        if (end == std::string_view::npos) end = s.size();
        std::string_view tok = s.substr(start, end - start);
        // trim
        std::size_t a = 0;
        while (a < tok.size() && std::isspace(static_cast<unsigned char>(tok[a]))) ++a;
        std::size_t b = tok.size();
        while (b > a && std::isspace(static_cast<unsigned char>(tok[b - 1]))) --b;
        tok = tok.substr(a, b - a);
        if (!tok.empty()) {
            std::int64_t id = 0;
            const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), id);
            if (ec == std::errc{} && ptr == tok.data() + tok.size() && id > 0) {
                if (!seen.contains(id)) {
                    seen[id] = true;
                    out.push_back(id);
                }
            }
        }
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

// 纯函数：从会话偏好恢复 TopTabState（P1-B0.5）。openCsv = sessionPreference("open_projects")，
// activeStr = sessionPreference("active_project")，existingIds = g_requests.allProjects() 的 id 集合。
// 流程：解析去重 → 按 existingIds 过滤（已删除/不可访问丢弃）→ 若过滤后为空但 active 有效且存在则自动补一个标签（保证活动项目可见）；
// 活动标签：active 在过滤后列表中则激活它，否则若列表非空激活首项，否则回主页；last_project 随活动项目同步。
inline TopTabState RestoreTopTabs(const std::string& openCsv, const std::string& activeStr,
                                 const std::vector<std::int64_t>& existingIds) {
    TopTabState s;
    const std::vector<std::int64_t> parsed = ParseOpenProjectsCsv(openCsv);
    std::unordered_map<std::int64_t, bool> existSet;
    for (std::int64_t id : existingIds) existSet[id] = true;
    for (std::int64_t id : parsed) {
        if (existSet.contains(id)) s.open_projects.push_back(id);
    }
    // 解析 active
    std::int64_t activeId = 0;
    {
        std::string_view t(activeStr);
        std::size_t a = 0;
        while (a < t.size() && std::isspace(static_cast<unsigned char>(t[a]))) ++a;
        std::size_t b = t.size();
        while (b > a && std::isspace(static_cast<unsigned char>(t[b - 1]))) --b;
        t = t.substr(a, b - a);
        if (!t.empty()) std::from_chars(t.data(), t.data() + t.size(), activeId);
    }
    const bool activeExists = activeId != 0 && existSet.contains(activeId);
    if (s.open_projects.empty() && activeExists) {
        s.open_projects.push_back(activeId);
    }
    if (!s.open_projects.empty()) {
        const bool activeInTabs =
            std::ranges::find(s.open_projects, activeId) != s.open_projects.end();
        if (activeExists && activeInTabs) {
            s.active = TopTabId{TopTabKind::Project, activeId};
            s.last_project = activeId;
        } else {
            s.active = TopTabId{TopTabKind::Project, s.open_projects.front()};
            s.last_project = s.open_projects.front();
        }
    } else {
        s.active = TopTabId{};
        s.last_project = 0;
    }
    // settings_open 始终 false（不持久化，启动无设置标签）
    s.settings_open = false;
    return s;
}

// common.cpp
huxerui::View PageHeader(std::string title, std::string subtitle);
huxerui::View MigrationPlaceholder(std::string pageName);
enum class AppIconButtonShape {
    Circular,
    RoundedSquare,
    Bare,
};

// 统一图标动作：glyph 只负责绘制（固定字号 font_size::kIconGlyph），命中区固定为
// icon_button_compact(28) / icon_button_regular(32) 两档——size 参数只认这两档，
// 其他值按就近档位收敛（>=30 归 regular），形状不再由文本尺寸推导。
// semanticLabel 是 icon-only 动作的可访问名称，同时作为 Tooltip 文本（官方 Tooltip
// modifier，§5.4"图标按钮必须提供 Tooltip 和可访问名称"）；所有形状（含 Bare）的
// hover/press indication 覆盖整个命中区。Bare 常态透明（配合外部悬停显隐或常驻
// 入口），hover 时显示整块圆角方形底。
huxerui::View AppIconButton(std::string glyph, std::string semanticLabel,
                            std::function<void()> onClick,
                            AppIconButtonShape shape = AppIconButtonShape::RoundedSquare,
                            float size = 28.0F, bool accent = false, bool enabled = true);
// 列表行尾部固定动作区（P1-A1 最小实现）：槽位按 icon_button_regular 档固定
// （每槽 32×32pt、间距 4pt），动作整体右对齐，槽宽不随 glyph/标签内容抖动。
// 契约：同一列表所有行的动作列必须等宽（行高对齐、文字不左右跳动）；传入的
// actions 应为 AppIconButton 等正方形命中区组件，本组件只负责占位与排布，
// 不接管点击语义（行选择与动作点击仍是独立事件目标）。
huxerui::View TrailingActionGroup(std::vector<huxerui::View> actions);
// "⋮" 语义图标按钮（AppIconButton Bare + compact 档，语义标签"更多操作"）：
// 只封装已有的菜单触发回调，由调用方在 onClick 里自己 UsePopup/ShowPopupMenu，
// 本组件不引入新菜单数据模型。enabled=false 时点击空转、外观降透明。
huxerui::View OverflowButton(std::function<void()> onOpenMenu, bool enabled = true);

// 大型自适应输入表面的语义状态（island-structure-theme.md §5.2）。tone 只改变
// 描边/提示色，**不改变整体几何**（圆角、内边距、高度在任意 tone 下完全一致）。
// 聚焦通常不需要调用方操心：surface 自动跟踪 body 自身的
// ViewEvents::FocusChanged（body 直接是输入控件时生效）；body 无法上报焦点
// （如 body 是包裹容器、或 SweetEditor 等非标准输入）时，才显式传
// InputSurfaceTone::Focused 点亮。优先级：
// Disabled > Error > Busy > （Focused 或自动跟踪的聚焦）> Normal。
enum class InputSurfaceTone {
    Normal,   // 常态：柔和描边（outline_soft）
    Focused,  // 聚焦（显式）：主色描边（焦点边界）
    Error,    // 校验失败：error 描边
    Disabled, // 禁用：半透明柔和描边（最弱）
    Busy,     // 发送中/处理中：主色弱化描边（几何不变，不阻塞 body 交互）
};

// 大型自适应输入表面（P1-A3 最小可用原语，island-structure-theme.md §5.2；
// **首批预期用途：agent/命令输入、全局搜索**——当前仓库尚无真实接入场景，
// 按 §12.4 只交付可复用原语、不虚构新 UI 入口、不接到任何页面）。
//
// 契约：
// - 单行（multiline=false）= full capsule 大胶囊；多行 = 固定 large_control_radius
//   (12pt) 大圆角，四角半径固定、四边随内容增长。这是单行↔多行切换时唯一的
//   几何差异。
// - body/trailing 的水平内边距、间距、尾部动作的右对齐锚点在两态下完全一致，
//   切换不产生横向跳动；trailing（尾部动作区）在两态均垂直居中，锚点稳定。
// - 正文区域高度被钳制在 [minBodyHeight, maxBodyHeight]：内容增长时表面随之
//   长高，到 maxBodyHeight 后**只滚动正文区域**（body 包在垂直 ScrollView 里，
//   两态树结构相同），不再扩大整个容器。
// - 聚焦/错误/禁用/发送中只改变描边 tone（见 InputSurfaceTone）。
// - body 会获得 Grow(1) 并被拉伸填满剩余宽度；trailing 为空传 huxerui::Row{}。
//   多行输入建议 body 只给 TextFieldLineLimits::MultiLine(minimum) 下限、不自带
//   上限——最大高度截断由本表面的 ScrollView 负责，避免双重封顶打架。
// - 实现只用 Row/Column、ScrollView、Frame、Padding、Border/CornerRadius 与
//   ProvideEnvironment 可表达的原语，不含九宫格/自定义渲染。
huxerui::View AdaptiveInputSurface(huxerui::View body, huxerui::View trailing,
                                   bool multiline,
                                   InputSurfaceTone tone = InputSurfaceTone::Normal,
                                   float minBodyHeight = 32.0F,
                                   float maxBodyHeight = 160.0F);
// 最小岛屿结构原语：Surface 负责语义表面、圆角、边界和内边距；Section 在其上
// 提供标题/说明/内容的稳定排版。复杂页面模板待真实迁移验证后再抽取。
huxerui::View IslandSurface(huxerui::View content, IslandLevel level = IslandLevel::Base);
huxerui::View IslandSection(std::string title, std::string description,
                            huxerui::View content);
huxerui::View IslandDialog(huxerui::View content);
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
// MenuItem：label + 点击回调 + 可选选中态 + 危险模式 + children（级联子菜单）+
// separator_before（本行上方画 1pt 分隔线，用于分组条目）；
// 外观对齐环境 MenuStyle，
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
    // 子菜单（级联飞出，行尾带 ›）：hover/点击在右侧弹子层。父项若设
    // on_click，点击 = 执行回调并关闭整链（直达该层级的动作，如"移动到
    // 分组 A"本身也是一个目的地）；不设则点击仅展开。
    std::vector<PopupMenuItem> children;
    // true = 本行上方画 1pt 分隔线（outline 色、随菜单宽度拉满），用于把
    // 条目分成视觉组（如"＋ 新建"菜单的请求类型/目录/导入三段）。
    bool separator_before = false;
};
void ShowPopupMenu(huxerui::PopupHandle popup, std::vector<PopupMenuItem> items,
                   const huxerui::PopupOptions& options = {});
void ShowPopupMenuAt(huxerui::PopupHandle popup, huxerui::Point point,
                     std::vector<PopupMenuItem> items,
                     const huxerui::PopupOptions& options = {});

// P1-B1.2 统一菜单模型（一级/二级，应用层；渲染经 ShowAppMenu 适配 ShowPopupMenu）
// 页面不再手写 PopupMenuItem 布局，统一经 AppMenuItem 表达，再由渲染器转 PopupMenuItem。
// 三级在模型上允许，UI 按需扁平化；真实三级仅在无法扁平时实现。
enum class AppMenuTone {
    Default,    // 常规
    DangerHover, // 危险：常态普通色，hover 变红（PopupMenuDanger::kHoverRed）
    DangerAlways, // 危险常驻红（kAlwaysRed）
};
struct AppMenuItem {
    std::string label;
    std::function<void()> onClick;
    bool enabled = true;
    bool checked = false;
    AppMenuTone tone = AppMenuTone::Default;
    std::optional<huxerui::Color> labelColor;
    std::optional<huxerui::ImageResource> icon;
    std::optional<std::string> shortcut;
    std::vector<AppMenuItem> children;
    bool separatorBefore = false;
    std::string id; // 稳定 id，默认 label
};
// 统一渲染：AppMenuItem → PopupMenuItem → ShowPopupMenu[At]
void ShowAppMenu(huxerui::PopupHandle popup, std::vector<AppMenuItem> items,
                 const huxerui::PopupOptions& options = {});
void ShowAppMenuAt(huxerui::PopupHandle popup, huxerui::Point point, std::vector<AppMenuItem> items,
                   const huxerui::PopupOptions& options = {});
// Hover 触发菜单专用：仍使用全局菜单主题，额外回传菜单表面 Enter/Leave，调用方可
// 与触发器 hover 合并决定何时关闭。返回 LayerId 供受控 Dismiss。
huxerui::LayerId ShowHoverAppMenu(huxerui::PopupHandle popup, std::vector<AppMenuItem> items,
                                  std::function<void(bool)> onMenuHover,
                                  const huxerui::PopupOptions& options = {});
// SweetEditor 调色板（apitab 本地补丁新增的 SweetEditorOptions::palette）：默认构造
// = 上游浅色主题。浅色主题直接返回默认；深色主题返回一套以 apitab 深色令牌为骨架
// （结构色取自 ThemeSpec，语法/补全强调色取 VS Code Dark+ 近似值）的配色。
sweetedit_huxer::SweetEditorPalette EditorPalette(const huxerui::ThemeSpec& theme);
// 方法 + URL 合并控件（Postman 风格）：左侧扁平方法选择（文本 ▾ 弹菜单），
// 之后是当前环境 baseUrl 显示区（灰色只读、可截断；为空则不渲染；URL 输入自带
// URI scheme 时弱化显示表示不拼接），1pt 分隔线，右侧 URL 输入，整体共用一个
// 10–12pt 大圆角描边外框（large_control_radius 组合栏，不做 full capsule，见
// §5.2）。自带 Grow(1)。
huxerui::View MethodUrlBar(std::vector<std::string> methods, std::size_t methodIndex,
                           std::function<void(std::size_t)> onMethodChanged,
                           huxerui::TextEditingValue url,
                           std::function<void(const huxerui::TextEditingValue&)> onUrlChanged,
                           std::string baseUrl, std::string placeholder);

// home_page.cpp — 主页（顶级 Home 标签内容，全宽无侧栏）。onOpenProject 由 AppRoot
// 注入：ProjectCard 的推迟任务在完成领域写入（selectProjectInOrg/setProject/
// active_project 持久化）后调用，内部完成顶级项目标签的新增/激活 + State 写回 +
// open_projects 按需持久化（见 app.cpp AppRoot；调用方必须已在推迟语境，CLAUDE.md
// 约定 6）。activeProject 仅用于 is_open 高亮（领域打开态）。
huxerui::View HomePage(std::function<void(std::int64_t)> onOpenProject,
                       huxerui::State<std::int64_t> activeProject);

// request_page.cpp — 请求工作区根编排（薄组合：左岛集合树 + 右岛 HTTP/WS/TCP/gRPC 分派）。
huxerui::View RequestPage(huxerui::State<std::int64_t> activeProject);

// request_body_editor.cpp — Body 文本编辑器与格式化 helpers（P1-C1 自 request_page.cpp
// 拆出）：SweetEditor 代码编辑器（行号/语法高亮/等宽，palette 跟随主题）+ JSON 注释
// 剥除 + XML 美化。BodyTextEditor 为跨 TU composable；SyntaxForBodyKind/
// StripJsonComments/PrettyXml 为普通函数（签名仅用 std 类型，可安全进头文件），
// 供 RequestEditor 格式化路径调用。
huxerui::View BodyTextEditor(huxerui::State<std::vector<RequestDraft>> drafts,
                             std::size_t index, const RequestDraft& snapshot,
                             const huxerui::ThemeSpec& theme,
                             sweetedit_huxer::SweetEditorController controller);
std::string_view SyntaxForBodyKind(std::size_t kind);
std::string StripJsonComments(const std::string& in);
std::string PrettyXml(const std::string& input);

// request_doc.cpp — 请求文档页（P1-C1 自 request_page.cpp 拆出）：按当前草稿只读
// 生成方法/URL/KV/Body 文档，State 变化即重组刷新。
huxerui::View RequestDocPage(const RequestDraft& snapshot, const std::string& envBaseUrl);

// request_response.cpp — 请求工作区右侧下岛（P1-C1 自 request_page.cpp 拆出）：响应区
// Body/Headers/Cookies 三档切换 + 内部滚动。State 订阅局限在岛内，不扩散到编辑器。
huxerui::View ResponseArea(huxerui::State<std::string> responseBody,
                           huxerui::State<std::vector<std::string>> responseHeaders,
                           huxerui::State<std::vector<std::string>> responseCookies,
                           const huxerui::ThemeSpec& theme);

// request_list.cpp — 请求工作区左岛（P1-C1 自 request_page.cpp 拆出）：当前项目
// 请求集合树（分组折叠 / 请求叶子，行尾 ⋮ / 右键统一菜单、拖拽移入分组或根）。
// vertical=true 用于 Compact 视口（列表改顶部横岛，限高撑宽）。
huxerui::View RequestListIsland(huxerui::State<std::vector<RequestDraft>> drafts,
                                huxerui::State<std::size_t> activeTab,
                                huxerui::State<int> listVersion, bool vertical);

// request_page.cpp（P1-C1 起外部链接）— 导入接口弹窗内容：左岛“+”菜单 → 导入
// 接口…；文件选择（FilePicker）或粘贴退化 + OpenAPI/Postman 解析预览 + 同步落库。
// 依赖的 ImportedBodyKindIndex/InferKvType 仍为 request_page.cpp 私有。
huxerui::View ApiImportDialogContent(huxerui::DialogContext ctx, huxerui::State<int> listVersion);

// testcase_page.cpp — 请求编辑器子页"测试用例"（pageTab=2）：用例编辑与
// 断言运行，读写草稿 cases（持久化经 request_page 保存按钮全量落库）。
huxerui::View TestCasePage(RequestDraft snapshot,
                           huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index);
// mock_page.cpp — 请求编辑器子页"Mock"（pageTab=3）：模拟响应定义，读写草稿
// mock；调试页"发送"在 mock.enabled 时拦截为本地模拟响应。
huxerui::View MockPage(RequestDraft snapshot,
                       huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index);

// loadtest_page.cpp
huxerui::View LoadTestPage();

// websocket_page.cpp
huxerui::View WebSocketPage();

// tcp_page.cpp
huxerui::View TcpPage();

// history_page.cpp
huxerui::View HistoryPage();

// settings_page.cpp（全局设置：主题模式 + 关闭行为，状态由 AppRoot 持有）
huxerui::View GlobalSettingsPage(huxerui::State<int> themeMode, huxerui::State<int> closeBehavior,
                               huxerui::State<std::size_t> category);

// project_settings_page.cpp（当前项目设置）
huxerui::View ProjectSettingsPage();

// http_test_page.cpp（框架自带 HTTP + 协程的并发压测实验页）
huxerui::View HttpTestPage();

} // namespace apitab::ui
