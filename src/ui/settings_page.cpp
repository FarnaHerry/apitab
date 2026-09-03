// settings_page.cpp — 全局设置工作区（顶级"设置"单例标签的内容，AppRoot 全宽
// 渲染）。岛屿式双岛布局（island-structure-theme.md §13.3）：左侧分类岛
// （通用 / 外观 / 关于，固定宽不随内容滚动）+ 右侧内容岛（独立滚动）；
// Compact 视口下左栏折叠为内容岛上方的横向三分段条（官方 SegmentedButton，
// 不引入 Drawer），标准/Compact 共用同一个分类 State。
//
// 受控值以应用状态为权威（CLAUDE.md 约定 5）：themeMode/closeBehavior 由
// AppRoot 持有原样传入；timeoutSec 与分类 State 都挂在
// GlobalSettingsPage 顶层——切换分类只重组内容岛，不重置任何未提交输入。
// 分类 State 随设置标签关闭（子树卸载）销毁：再打开固定回到"通用"，这是
// §13.3 "上次会话内分类或通用"二选一的**固定行为**；主题切换只重组不重挂载
// （内容子树 Key 不含主题，§13.2 项 7），分类与受控输入跨主题存活。
//
// 选择都持久化到 settings.ini（会话偏好）；超时写入
// session.request_timeout_sec，发送侧在 store 的 finalizeSpec 统一读取生效
//（见 src/store/requests.cppm）。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui.h"

import apitab.config;
import apitab.preferences;

// 应用版本：CMake 注入（CMakeLists.txt 的 target_compile_definitions
// APITAB_VERSION="${PROJECT_VERSION}"，随顶层 project(VERSION) 自动同步）；
// 未注入（如脱离构建系统的单文件检查）时兜底 "dev"。
#ifndef APITAB_VERSION
#define APITAB_VERSION "dev"
#endif

namespace apitab::ui {

namespace {

// ---- 分类模型（island-structure-theme.md §13.3）---------------------------
// 下标即分类 State 值；标准/Compact 两种形态共用同一 State。
constexpr std::size_t kCategoryGeneral = 0;
constexpr std::size_t kCategoryAppearance = 1;
constexpr std::size_t kCategoryAbout = 2;
constexpr std::size_t kCategoryCount = 3;
constexpr std::string_view kCategoryNames[kCategoryCount] = {"通用", "外观", "关于"};

// 左栏固定宽档位：取 §13.3 允许的 176–208pt 档的中值 192pt——三字分类名加
// 行内边距舒适，且与主页 240pt 组织岛拉开层次。
constexpr float kCategoryRailWidth = 192.0F;

// 分类行：整行可点、统一选中/hover/focus 表面（参考 home_page OrgRow 的手工
// 悬停配方：显式压掉 OnClick 自动追加的默认 Indication，由手工底色承担整行
// 反馈，避免双层叠加）。取色经 ResolveIslandTheme：选中 = islands.active 一档、
// hover = islands.raised、常态透明；圆角取嵌套档 islands.nested_radius。
//
// 键盘能力（已查证 SDK 源码）：Row 挂 OnClick 只成为"可激活"节点
// （runtime.cpp IsActivatable = 有 activation 或 ViewEvents::Click 绑定），
// 但普通 View 默认不可聚焦 → 必须再挂框架 Focusable(true) 修饰符参与 Tab
// 焦点遍历（CollectFocusableNodes 按 focusable 收集，先序 = 声明顺序，故
// Tab 序 = 先分类后内容）。聚焦后 Enter/Space 激活由 runtime 键盘路径派发
// （Enter Down 非重复 → ActivateNode；Space Down/Up 配对 → ActivateNode，
// 均转为 ViewEvents::Click → OnClick，带 press/release 交互态），页面不写
// 按键路由。方向键不是全局焦点导航键（runtime 只处理 Tab/Shift+Tab），左栏
// 上下键移动为 SDK 能力缺口，如实记录、不造临时实现。
[[huxerui::composable]] huxerui::View SettingsCategoryRow(std::string_view label, bool selected,
                                                          std::size_t index,
                                                          huxerui::State<std::size_t> category) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto tasks = huxerui::UseTaskScope();
    auto hovered = huxerui::UseState(false);
    return huxerui::Row {
        huxerui::Text(std::string(label), huxerui::TextRole::Label)
            .With(huxerui::Grow(1.0F),
                  huxerui::Foreground(selected ? theme.colors.on_surface
                                               : theme.colors.on_surface_variant)),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(12.0F, 9.0F)),
              huxerui::CornerRadius(islands.nested_radius),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              selected ? huxerui::Background(islands.active)
                       : hovered.Get() ? huxerui::Background(islands.raised)
                                       : huxerui::Background(huxerui::Color::Transparent()),
              huxerui::Indication{}, // 有意压制默认 Indication（见上），整行反馈 = 手工底色
              huxerui::Focusable(true),
              // 可访问名称：整行一个 Button 语义，屏幕阅读器朗读分类名。
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = std::string(label)})
        .OnClick([tasks, category, index] {
            // 分类切换会重组卸载右侧内容分区（本行所在的左栏不受影响），按
            // CLAUDE.md 约定 6 统一推迟出指针/按键事件路径。
            tasks.Launch([category, index]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                category = index;
            });
        })
        .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& e) {
            if (e.type == huxerui::HoverEventType::Enter)
                hovered = true;
            else if (e.type == huxerui::HoverEventType::Leave)
                hovered = false;
        });
}

// 左侧分类岛：固定宽（kCategoryRailWidth），不随右侧长表单滚动。行 Key 用
// 分类下标（稳定、与顺序一致），选中变化原位更新不重挂载。
[[huxerui::composable]] huxerui::View SettingsCategoryIsland(huxerui::State<std::size_t> category) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const std::size_t current = category.Get();
    std::vector<huxerui::View> rows;
    rows.reserve(kCategoryCount);
    for (std::size_t i = 0; i < kCategoryCount; ++i) {
        rows.push_back(
            SettingsCategoryRow(kCategoryNames[i], current == i, i, category).Key(i));
    }
    return IslandSurface(huxerui::Column(std::move(rows))
                             .With(huxerui::Spacing(theme.spacing.extra_small),
                                   huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
                         IslandLevel::Base)
        .With(huxerui::Frame{.width = kCategoryRailWidth});
}

// 设置分组：标题 + 控件 + 说明，以标题与留白分组，不逐项套卡（§13.3）。
[[huxerui::composable]] huxerui::View SettingsGroup(std::string title, huxerui::View control,
                                                    std::string note) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text(std::move(title), huxerui::TextRole::Label),
        std::move(control),
        huxerui::Text(std::move(note), huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 通用分区：关闭行为、默认请求超时、数据目录 -------------------------
[[huxerui::composable]] huxerui::View GeneralSettingsSection(
    huxerui::State<int> closeBehavior,
    huxerui::State<huxerui::TextEditingValue> timeoutSec) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        PageHeader("通用", "应用级默认行为，保存于 settings.ini。"),
        // 关闭行为：受控 Select，值由 AppRoot 持有传入。
        SettingsGroup("关闭行为",
                      huxerui::Row {
                          huxerui::Select(std::vector<std::string>{"每次询问", "直接关闭", "最小化到托盘"},
                                          static_cast<std::size_t>(closeBehavior.Get()),
                                          [](const std::string& option) {
                                              return huxerui::Text(option).Key(option);
                                          })
                              .OnChanged([closeBehavior](std::size_t index) {
                                  closeBehavior = static_cast<int>(index);
                                  saveSessionPreference("close_behavior",
                                                        std::to_string(closeBehavior.Get()));
                              }),
                      },
                      "点标题栏 ✕ 时生效；“每次询问”在关闭时弹窗确认。"),
        // 默认请求超时：数字文本承载（受控 TextEditingValue 全量保留），初始读
        // session.request_timeout_sec，空/非法在发送侧按 30 秒兜底。OnChanged 即
        // 写回 ini（写文件很轻，不必防抖）。
        SettingsGroup("默认请求超时（秒）",
                      huxerui::TextField(timeoutSec.Get())
                          .Label("秒")
                          .Placeholder("30")
                          .Variant(huxerui::TextFieldVariant::Outlined)
                          .OnChanged([timeoutSec](const huxerui::TextEditingValue& value) {
                              timeoutSec = value;
                              saveSessionPreference("request_timeout_sec", value.text);
                          })
                          .With(huxerui::Frame{.max_width = 240.0F}),
                      "作用于单次 HTTP 请求（curl 引擎），k6 压测不受影响；留空或非法按 "
                      "30 秒，下次发送即生效。"),
        // 数据目录：用 cfg::dataDir() 取真实路径（与 SQLite 落盘位置同一来源，
        // 不写死第二份）。
        SettingsGroup("数据目录",
                      huxerui::Text(cfg::dataDir().string(), huxerui::TextRole::Body),
                      "SQLite 数据库与 settings.ini 的存放位置。"),
        // 底部栏快捷入口过渡文案（与 P1-B1 状态条计划一致；底部栏按钮本体的
        // 改名属 P1-B1，不在本页改）。
        huxerui::Text("请求代理（应用级）与项目 Cookie 的快捷配置暂在窗口底部栏"
                      "「请求代理」「全局 Cookie」按钮；按作用域拆分状态条的改版"
                      "（应用级配置并入本页、项目级进入项目上下文条）将在后续版本"
                      "进行。",
                      huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 外观分区：主题模式 + 圆形揭示过渡（动画逻辑原样保留）-----------------
[[huxerui::composable]] huxerui::View AppearanceSettingsSection(
    huxerui::State<int> themeMode, const std::function<void(int)>& applyTheme) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        PageHeader("外观", "主题外观与界面偏好。"),
        // 官方下拉选择（下标即 themeMode：0=跟随系统 1=深色 2=浅色）；动画
        // 原点 = 点中菜单项的指针位置，切换后仍停留在外观分类。
        SettingsGroup("主题模式",
                      huxerui::Row {
                          huxerui::Select(std::vector<std::string>{"跟随系统", "深色模式", "浅色模式"},
                                          static_cast<std::size_t>(themeMode.Get()),
                                          [](const std::string& option) {
                                              return huxerui::Text(option).Key(option);
                                          })
                              .OnChanged([applyTheme](std::size_t index) {
                                  applyTheme(static_cast<int>(index));
                              }),
                      },
                      "跟随系统会读取当前桌面的深浅色偏好。界面密度与动效偏好将在"
                      "后续版本提供。"),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

// ---- 关于分区：版本、构建与许可证、数据与文档位置（全部可验证信息，不虚构
// 在线更新/遥测/账户能力）----------------------------------------------------
// HuxerUI 版本：SDK 头没有版本常量（third_party/huxerui include/ 全量 grep
// 已核实），用常量对齐 third_party/tarballs 的 SDK 包与 third_party/huxerui
// 源码 project(VERSION 0.2.0)；SDK/源码升级时必须同步本常量。
constexpr std::string_view kHuxerUiVersion = "0.2.0";

// 关键第三方依赖及许可证类型（版本清单以 third_party/README.md 为唯一来源，
// 此处不重复版本号以免失同步）。许可证逐一核对自各依赖源码树：
// third_party/huxerui/LICENSE（MIT）、huxerui-sweetedit 3dparty/*/LICENSE
// （MIT）、vendored 包 COPYING/LICENSE.txt（asio BSL-1.0、IXWebSocket
// BSD 三条款、curl 许可证、SQLiteCpp MIT）、json.hpp SPDX（MIT）；
// OpenSSL 3.x 为 Apache-2.0。
struct AboutDependency {
    std::string_view name;
    std::string_view license;
};
constexpr AboutDependency kAboutDependencies[] = {
    {"HuxerUI（GUI 框架）", "MIT"},
    {"SweetEditor / SweetLine（代码编辑器）", "MIT"},
    {"Asio（网络）", "Boost 软件许可证 1.0"},
    {"IXWebSocket（WebSocket）", "BSD 三条款"},
    {"curl（HTTP）", "curl 许可证"},
    {"SQLiteCpp（SQLite 封装）", "MIT"},
    {"nlohmann::json（JSON）", "MIT"},
    {"OpenSSL（TLS）", "Apache-2.0"},
};

[[huxerui::composable]] huxerui::View AboutSettingsSection() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    std::vector<huxerui::View> infoLines;
    infoLines.push_back(
        huxerui::Text(std::string{"apitab 版本："} + APITAB_VERSION, huxerui::TextRole::Body));
    infoLines.push_back(huxerui::Text(std::string{"HuxerUI（GUI 框架）版本："} +
                                          std::string(kHuxerUiVersion),
                                      huxerui::TextRole::Body));
    for (const AboutDependency& dep : kAboutDependencies) {
        infoLines.push_back(
            huxerui::Text(std::string(dep.name) + " — " + std::string(dep.license),
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }
    return huxerui::Column {
        PageHeader("关于", "应用与运行环境的版本及依赖信息。"),
        SettingsGroup("版本", huxerui::Column(std::move(infoLines))
                                  .With(huxerui::Spacing(theme.spacing.extra_small)),
                      "版本号随构建注入（顶层 project(VERSION)），HuxerUI 版本对齐 "
                      "third_party 内的 SDK/源码。"),
        SettingsGroup("构建与许可证",
                      huxerui::Text("apitab 以 MIT 许可证发布；依赖清单与版本见仓库 "
                                    "third_party/README.md。",
                                    huxerui::TextRole::Body),
                      "以上各依赖的许可证类型见版本组列表，均允许随应用再分发。"),
        SettingsGroup("数据与文档位置",
                      huxerui::Column {
                          huxerui::Text(std::string{"数据目录："} + cfg::dataDir().string(),
                                        huxerui::TextRole::Body),
                          huxerui::Text("使用文档在仓库 docs/ 目录：docs/apitab-cli.md"
                                        "（CLI 用法）、docs/huxerui-migration.md"
                                        "（HuxerUI 迁移说明）、docs/plans/（设计与实施"
                                        "计划）。",
                                        huxerui::TextRole::Body)
                              .With(huxerui::Foreground(theme.colors.on_surface_variant)),
                      }
                          .With(huxerui::Spacing(theme.spacing.extra_small)),
                      "数据目录与「通用」分区显示的是同一来源（cfg::dataDir()）。"),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]] huxerui::View GlobalSettingsPage(huxerui::State<int> themeMode,
                                                         huxerui::State<int> closeBehavior,
                                                         huxerui::State<std::size_t> category) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    // 场景过渡：主题切换（整树换色）用圆形揭示动画包一层；reduced motion
    // 由 handle 自行降级，mutation 在无视觉效果时也必须正确落盘。
    auto transition = huxerui::UseSceneTransition();
    // 动画冻结标记：场景过渡没有"运行中"查询/完成回调，重复触发会打断上一个
    // 动画（很难看）。app 侧用定时器（略长于默认 Tween 0.36s）模拟冷却期，
    // 期间按钮再按无反应。
    struct FlagCell {
        bool animating = false;
    };
    auto animating = huxerui::UseState(std::make_shared<FlagCell>());

    // 受控 State 全部挂在页面顶层（非分区内）：分类切换只改 category，其它
    // State 原样存活——切分类不重置任何未提交的受控输入（§13.3）。
    // 默认请求超时：初始读 session.request_timeout_sec。
    auto timeoutSec = huxerui::UseState(
        huxerui::TextEditingValue {sessionPreference("request_timeout_sec")});
    // 分类 State 由 AppRoot 持有（P1-B0.5 状态保活）：设置标签仍打开但切到项目后
    // 再切回保留原分类；未保存请求草稿的保活见 draftMap（AppRoot）。
    // 主题切换只重组不重挂载（AppRoot 渲染键不含主题），分类跨主题存活（§13.2 项 7）。
    // 不再在页面内 UseState，否则切到项目即卸载、再打开固定回"通用"丢失状态。

    // 切换主题模式（0=跟随系统 1=深色 2=浅色）。检测当前有效深浅：
    // 目标与现状一致（含"跟随系统"已等于系统偏好）时直接落盘、不放动画。
    // 动画原点 = 本次点击的指针位置（RunFromCurrentInteraction 取当前事件
    // 派发的精确坐标；键盘激活时回落到锚点 View 中心）。深→浅的"收束"方向
    // 上游刻意不提供（场景过渡不支持反向播放），两个方向都用展开。
    auto applyTheme = [themeMode, transition, tasks, animating](int mode) {
        if (animating.Get()->animating) return; // 动画进行中：冻结，再按无反应
        const bool currentDark =
            themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
        const bool targetDark = mode == 1 || (mode == 0 && cfg::systemPrefersDark());
        auto mutation = [themeMode, mode] {
            themeMode = mode;
            saveSessionPreference("theme_mode", std::to_string(themeMode.Get()));
        };
        if (currentDark == targetDark) {
            mutation();
            return;
        }
        animating.Get()->animating = true;
        tasks.Launch([animating]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0.5});
            animating.Get()->animating = false;
        });
        // 必须在同步事件回调内调用（Select OnChanged 满足）；mutation 由过渡
        // 服务在换帧点调用（已在指针事件路径之外），无需再手动推迟。
        transition.RunFromCurrentInteraction(huxerui::CircularRevealSceneTransition{},
                                             std::move(mutation));
    };

    // 按当前分类渲染对应分区；分区受控 State 在页面顶层，分类切换不丢输入。
    huxerui::View section;
    switch (category.Get()) {
    case kCategoryAppearance:
        section = AppearanceSettingsSection(themeMode, applyTheme).Key(kCategoryNames[1]);
        break;
    case kCategoryAbout:
        section = AboutSettingsSection().Key(kCategoryNames[2]);
        break;
    case kCategoryGeneral:
    default:
        section = GeneralSettingsSection(closeBehavior, timeoutSec).Key(kCategoryNames[0]);
        break;
    }

    // 页面根：标准 = 双岛（左分类岛 + 右内容岛）；Compact = 单内容岛 + 岛内
    // 顶部分类选择器。section 只在当前分支消费一次（两分支各自组岛）。
    huxerui::View page;
    if (huxerui::UseViewportClass() == huxerui::ViewportClass::Compact) {
        // Compact：左栏折叠为内容岛内的顶部分类选择器（官方 SegmentedButton，
        // 横向三分段条，不引入 Drawer）。放在岛内而非海面上——控件不漂浮在
        // 海面（岛屿设计语言）；选择器在滚动内容上方、不随之滚走。与标准
        // 形态共用同一个分类 State，键盘 Left/Right/Home/End 由组件自带。
        page = IslandSurface(huxerui::Column {
                                 huxerui::SegmentedButton(
                                     std::vector<huxerui::StringVariant>{
                                         kCategoryNames[0], kCategoryNames[1],
                                         kCategoryNames[2]},
                                     category.Get())
                                     .OnChanged([tasks, category](std::size_t index) {
                                         // 分段条与内容分属不同子树，本组件切换分类
                                         // 不卸载自身；仍按约定 6 推迟出事件路径。
                                         tasks.Launch([category, index]() -> huxerui::Task<void> {
                                             co_await huxerui::Delay(
                                                 std::chrono::duration<double>{0});
                                             category = index;
                                         });
                                     }),
                                 huxerui::ScrollView{std::move(section)}
                                     .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
                             }
                                 .With(huxerui::Spacing(theme.spacing.medium),
                                       huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                                       huxerui::Grow(1.0F)),
                             IslandLevel::Base)
                 .With(huxerui::Grow(1.0F));
    } else {
        // 标准：右侧内容岛——页面标题固定在顶部（不随内容滚走），分区内容独立
        // 滚动（ScrollView + ScrollBar + Grow）。左右两块属于同一个设置工作区，
        // 只用 small 间距（8pt），不套页面级 page_gap；避免两个相邻岛被拉得过散。
        huxerui::View contentIsland = IslandSurface(
            huxerui::Column {
                huxerui::ScrollView{std::move(section)}
                    .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Grow(1.0F)),
            IslandLevel::Base);
        page = huxerui::Row {SettingsCategoryIsland(category), std::move(contentIsland)}
                   .With(huxerui::Spacing(theme.spacing.small),
                         huxerui::Grow(1.0F),
                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }

    return std::move(page).With(
        huxerui::Grow(1.0F),
        // 过渡锚点：圆形揭示以本页面区域为画布（键盘激活时的原点兜底）。
        transition.Anchor());
}

} // namespace apitab::ui
