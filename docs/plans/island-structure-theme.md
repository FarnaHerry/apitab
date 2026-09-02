# 计划：岛屿式结构主题与统一控件几何

状态：**P0 已实施；P1-A 已实施；P1-B0 四工作包代码已落地（行为矩阵静态核验通过，
实机回归待人工，见 §13.6 后"P1-B0.4 收口结果"）；下一批 P1-B1「菜单统一与项目
上下文条」待启动；P2 待继续**。

已完成（2026-09-01）：

- 冷中性灰白浅色令牌与 `IslandTheme`（2026-09-02 已从偏黄米白重新校准）。
- `IslandSurface`、`IslandSection`、`IslandDialog`，全局设置页完成首个外壳迁移。
- `AppIconButton` 三种形状契约与 28/32pt 固定命中区。
- 高频列表 `⋮`、`✕`、`✎`、新建 `+` 的首批迁移；拖拽预览中的符号保持纯展示。
- 底部信息作用域审计：应用代理保持应用级；`GlobalCookie` 后续改名项目 Cookie；
  环境保持项目级；运行态另行拆分。

P1-A 已实施（2026-09-01，四工作包全部完成）：

- **P1-A1**：`IslandTheme` 新增几何令牌 `control_radius=8` / `large_control_radius=12` /
  `icon_button_compact=28` / `icon_button_regular=32` / `control_height=32`，新增
  `ConcentricRadius(outer, inset)`；`AppIconButton` 收束为 28/32 两档命中区、字形固定
  `font_size::kIconGlyph=15`、形状显式三选；新增 `TrailingActionGroup`（32pt 槽、间距 4、
  总宽固定）与 `OverflowButton`（⋮ Bare 28）。
- **P1-A2**：主页 `OrgRow` 与历史 `HistoryRow` 按"前置区—主内容—尾部信息—固定动作区"
  布局契约迁移；历史行整列表决定不放动作槽，尾部信息右对齐，动作列宽度不随内容抖动。
- **P1-A3**：新增 `AdaptiveInputSurface` + `InputSurfaceTone`（单行 full capsule ↔ 多行
  `large_control_radius`，零跳动，正文限高滚动，tone 只换描边色）；当前无真实
  agent/命令输入场景，按 §12.4 只交付原语、不接页面；`MethodUrlBar` 外框升级
  `large_control_radius`（签名未动，loadtest_page 调用点零改动兼容）；request_page
  四处图标动作迁移（CircleButton ×3、内联 ⋮ ×1 → AppIconButton/OverflowButton）。
- **P1-A4**：`CircleButton` 删除（调用点已于 A2/A3 清零，全仓只剩一套图标按钮体系）；
  Tooltip 判定为官方 `Tooltip` modifier 低成本可用、已单点接入 `AppIconButton`；
  全仓 `CornerRadius`/固定 `Frame`/文本按钮散落审计分三档（见 §12.2 P1-A4 收口结果）；
  修正 AppIconButton 非 Bare 形状无 hover/press 反馈的集成遗漏；
  `cmake --build build --target apitab -j2`、`ctest`、`git diff --check` 全部通过。

P1-B0 已实施（2026-09-02，四工作包全部完成；行为矩阵静态核验见 §13.6 后
"P1-B0.4 收口结果"，实机回归待人工）：

- **P1-B0.1**：`ui.h` 新增 `TopTabKind/TopTabId/TopTabState` 与纯 helper
  （ActivateHome/ActivateProject/ActivateSettings/CloseTopTab，回退矩阵写在
  CloseTopTab doc 注释）；AppRoot 集中持有 tabs/activeProject/settingsOpen/
  activeTopTab/lastProjectTab/navPage（navPage 只表达项目内部页，kHome/kAppSettings
  已从 PageIndex 删除），统一 commitTopTab/activateTopTab/closeTopTab；标签条泛化为
  TopTabStrip（主页钉最左、设置单例标签固定队尾、sentinel 显示 key = int64 max、
  ✕ 迁 AppIconButton(Bare,28)）；HomePage 新签名注入 onOpenProject；EmptyState
  死代码删除；内容 Key = 字符串（"home"/"settings"/"project:<id>:<navPage>"/
  "httptest"）。
- **P1-B0.2**：设置页三分区（通用/外观/关于）+ 左分类岛 192pt + 右内容岛独立滚动；
  Compact 用官方 SegmentedButton 顶部分段条；分类 State 在页顶层（重开固定回
  "通用"，受控输入跨分类切换保留）；键盘 Enter/Space 经 `Focusable(true)` 可用
  （方向键在标准左栏是 SDK 缺口、Compact 分段条可用）；About 版本经 CMake
  `APITAB_VERSION` 注入 + HuxerUI 0.2.0 常量 + 许可证逐一核对。
- **P1-B0.3**：环境 ☰ 迁 `AppIconButton("☰","环境配置",Bare,28)`，外包
  CrossAlign(Center) Row 防 Stretch 拉伸，Dialog 流程零变化；项目标签 ✕ 迁移由
  B0.1 一并完成。
- **P1-B0.4**：双状态竞争归零审计、§13.6 行为矩阵逐行静态核验、键盘可达性审计与
  Focusable/Semantics 最小修复（标签切换区/齿轮/闪电/HomePage 卡片/AppIconButton
  组件本体；✕ 键盘不可达为唯一缺口）、构建/ctest/diff-check/CLI 冒烟全部通过；
  详见 §13.6 后"P1-B0.4 收口结果"。

本文定义 apitab 后续 UI 收束方向。第一阶段统一岛屿式信息架构、冷中性灰白主题和
公共结构组件；第二阶段统一圆角、胶囊、小按钮、列表尾部动作和多级菜单行为。
本计划只记录设计与实施顺序，不代表当前代码已经符合这些约定。

## 一、设计目标

- 窗口背景是“海面”，岛屿只表达稳定的业务边界，不给每个控件重复套卡片。
- 浅色主题采用柔和的冷中性灰白，避免纯白生硬，也避免米白偏黄造成陈旧感。
- 内容层保持连续和安静；导航、工具和临时浮层才优先形成独立功能岛。
- 每类岛屿只有一种明确职责、内边距、圆角和层级，不允许页面自行拼装近似版本。
- 抽出公共结构组件后再迁移页面，禁止以复制 `Background + CornerRadius + Padding`
  的方式继续扩散局部样式。
- 控件几何由内容角色决定：胶囊、圆角矩形、圆形和无底图标按钮各司其职。
- 交互命中区与字形尺寸解耦；尤其 `⋮`、`+`、关闭等图标不能以文字本身作为按钮。
- 全局只保留真正跨项目的状态；代理、Cookie、环境等项目上下文进入项目工作区。

## 二、应用结构

```text
窗口海面 Ocean
├── 顶部导航岛 Top Dock
│   ├── Logo
│   ├── 主页与项目标签
│   └── 全局操作
├── 工作区
│   ├── 项目工具岛 Tool Rail
│   └── 页面画布
│       ├── 页面标题区
│       ├── 主工作岛 Primary Island
│       ├── 辅助区域 Secondary Region（必要时才成岛）
│       └── 浮动岛 Overlay Island
└── 项目状态岛 Project Status Dock（仅项目工作区出现）
```

### 2.1 海面与岛屿

- 海面负责窗口整体底色和岛间留白，不承载业务内容。
- 一级岛负责一块可独立理解、滚动或调整尺寸的业务区域。
- 二级分组优先使用标题、留白和分隔线，不默认形成嵌套岛。
- 只有需要独立交互状态、滚动边界或明显层级提升时才允许二级岛。
- 常态页面最多两级表面：海面和业务岛。第三级只给菜单、Dialog、拖拽预览等
  临时浮层，不把 `Raised` 当作常规分组手段。

### 2.2 停靠区域

- 顶部导航区统一为导航岛；活动项目标签可在岛内抬升，非活动标签不各自成卡。
- 左侧项目工具栏统一为窄工具岛，不再让图标无边界漂浮在海面上。
- 主页、全局设置等非项目页面不显示项目工具岛和项目状态岛。
- 当前底部条重新界定职责并按真实作用域拆分：环境和“全局 Cookie”（实际按项目存储，
  应改名“项目 Cookie”）属于项目；请求代理目前是应用级配置；连接/任务属于运行态。
- 真正全局的后台任务或应用更新提示若未来需要，应另设轻量全局通知机制，不能借用
  项目状态岛。

## 三、浅色主题采用柔和冷中性灰白

浅色方案不使用大面积纯白，也不依赖黄/棕色相制造柔和感。当前令牌如下，最终数值仍须
通过 Linux/Windows 实机视觉回归微调：

| 语义令牌 | 建议色值 | 用途 |
|---|---:|---|
| `ocean` | `#F3F4F6` | 窗口海面 |
| `island_base` | `#F8F9FA` | 一级业务岛 |
| `island_raised` | `#F1F3F5` | 少量工具面与层级提升 |
| `island_active` | `#E7EAEE` | 选中、活动标签、展开项 |
| `island_overlay` | `#FFFFFF` | Dialog、菜单、浮层 |
| `outline_soft` | `#D8DCE2` | 柔和边界与分隔线 |
| `text_primary` | `#24272C` | 主文本，避免绝对黑 |
| `text_secondary` | `#6B7280` | 次文本 |
| `accent` | `#25282D` | 主操作；保持克制的冷中性深灰 |

约束：

- 灰白只允许轻微冷倾向，不能出现明显蓝色；禁止重新加入黄/棕底色。
- 纯白仅允许用于高亮细节或必要的高对比内容，不作为默认岛面。
- 一级业务岛统一使用 `island_base`、16pt 圆角和中等内边距，常态不画外边框；
  设置页的分类岛与内容岛只是两个并列一级岛，不得使用页面私有的边框、圆角或大间距。
- 描边用于输入控件、焦点态和临时浮层，不用于给设置页一级岛额外套一层卡片感。
- 深色方案继续采用极简黑灰结构，但与浅色方案共享完全相同的语义令牌名称。
- HTTP 方法色、成功、警告和危险色保持语义独立，不参与大面积岛面铺色。
- SweetEditor 等嵌入式编辑器也从相同结构令牌派生背景和边界。

## 四、公共结构组件

公共组件建议落在 `src/ui/common.cpp` / `src/ui/ui.h`，主题定义留在应用主题边界附近；
若结构令牌继续增长，再拆分 `island_theme.h/.cpp`。UI 源仍须为普通 `.cpp`，遵守 hcg
扫描约束。先抽令牌和已被多个页面验证的原语，再从真实重复中提取页面模板，避免
一开始建立参数不断膨胀的万能组件。

### 4.1 结构令牌

```cpp
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
    float rail_width;
    huxerui::Color ocean;
    huxerui::Color base;
    huxerui::Color raised;
    huxerui::Color active;
    huxerui::Color overlay;
    huxerui::Color outline_soft;
};
```

页面不得直接用 `surface_container_low/high/...` 表达岛屿层级，而应通过语义组件和
`IslandLevel` 取色。

### 4.2 首批公共组件

- `IslandSurface(content, level)`：统一背景、圆角、内边距和边界。
- `IslandSection(title, description, content)`：设置分组和表单分组。
- `IslandDialog(content)`：替代并扩展当前 `DialogCard`。
- `AppIconButton(...)`：统一图标、命中区、语义和交互态。

`IslandPage`、`IslandSplitPane`、`IslandToolbar`、`IslandStatus` 和
`ProjectStatusDock` 先作为候选模式；至少完成两个真实页面迁移并确认参数稳定后再抽取。

### 4.3 页面模板

- 工作台页：左侧导航岛 + 主编辑岛 + 可折叠结果/辅助岛。
- 浏览页：轻量页面标题 + 筛选工具条 + 单一列表/网格主岛。
- 设置页：分类导航岛 + 设置内容主岛；设置项以留白和标题分组，不逐项套卡。

迁移期间不得形成“旧页面外壳岛 + 新公共岛”的双层嵌套。每迁移一页，应同步删除
该页原有的手工背景、圆角和外边距。

## 五、下一阶段：圆角统一与胶囊特殊化

### 5.1 几何层级

建议先建立统一几何令牌，具体数值在实现前以视觉样稿验证：

| 角色 | 建议几何 |
|---|---|
| 一级业务岛 | 16px 圆角 |
| 二级岛、浮动菜单 | 8–12px，按父级 inset 保持同心 |
| 普通按钮、选择器、局部分组 | 8px 圆角 |
| 大输入行 | 默认大胶囊 |
| 小型单图标按钮 | 正方形命中区；外观按角色选择圆形、圆角方形或无底 |
| 状态标签、方法标记 | 内容驱动的小胶囊 |

“胶囊”不再是所有按钮的默认造型，而是一种特殊语义：agent/命令输入、全局搜索、
短状态、短筛选和紧凑身份标记。普通动作按钮保持圆角矩形。嵌套圆角优先满足同心
关系：内层半径随父级半径与 inset 调整，而不是机械套用固定档位。

### 5.2 大而独立的输入行

参考 Codex 等 agent 软件的输入区体验，为 agent/命令输入和全局搜索抽出
`AdaptiveInputSurface`。请求 URL 是方法、地址、环境和发送动作组成的高密度工具条，
使用 10–12px 的大型圆角矩形组合栏，不默认套 full capsule：

- 单行状态为独立的大胶囊，输入正文、前置选择器和尾部动作共享同一外轮廓。
- 输入框内部控件不重复描边，不形成“胶囊里再套三个胶囊”。
- 内容跨行后不维持强行胶囊，而升级为自然拉伸的自适应圆角容器：四角半径固定，
  四边与中部自由增长。
- 单行与跨行切换不能跳动左右内边距，尾部操作区应保持稳定位置。
- 多行状态设置合理最小/最大高度，超过最大高度后仅正文区域滚动。
- 聚焦、校验失败、发送中和禁用状态只改变语义边界/状态提示，不改变整体尺寸。

这不构成九宫格位图或自定义布局要求；优先用现有 Row/Column、Frame、Padding、
TextField LineLimits 和圆角容器实现。单行可使用 full radius，多行切换为固定上限
半径，不能为了视觉模型引入不必要的渲染或测量抽象。

### 5.3 列表条与尾部动作

所有列表条采用共同布局契约：

```text
[前置图标/状态] [主标题 + 次信息……]    [尾部信息] [固定动作区]
```

- 主内容左对齐，尾部元信息和动作整体右对齐。
- 同类列表的固定动作区宽度一致，避免每行因内容不同而抖动。
- 右侧图标按钮使用统一正方形命中区和图标尺寸。
- `⋮` 按钮必须是按钮容器，不允许文字 `⋮` 本身直接承担点击：建议视觉图标 16px，
  命中区至少 28×28px，桌面舒适尺寸优先 32×32px。
- 按钮的 hover、press、focus 覆盖整个命中区，而不是只覆盖三个点。
- 无尾部按钮的行仍保留与同列表一致的尾部布局规则；是否保留占位宽度由列表模板决定。
- 行选择与尾部按钮点击必须是独立事件目标，点击 `⋮` 不触发行选择。

### 5.4 单独的小按钮

- 浮动或独立的 `+`、关闭等动作可使用完美圆形按钮。
- 工具栏和列表尾部的更多、刷新、展开等动作优先使用透明正方形命中区，hover 时显示
  圆角方形底；一组并排操作共享相同尺寸和视觉节奏。
- 同一密度下所有小按钮外框一致；字形大小不能反向决定按钮大小。
- 推荐默认命中区 32×32px，紧凑区最低 28×28px；图标通常 14–16px。
- 危险小按钮常态不必红底，可在 hover/确认阶段进入危险色；明确的最终确认按钮直接
  使用危险色。
- 图标按钮必须提供 Tooltip 和可访问名称。

统一组件支持三种外观：

- `AppIconButton(icon, label, shape, tone, size)`
- `IconButtonShape::{Circular, RoundedSquare, Bare}`
- `TrailingActionGroup(actions)`
- `OverflowButton(menuModel)`

现有 `CircleButton` 应迁移或删除，避免两个图标按钮体系并存（已于 P1-A4 删除，
见 §12.2 P1-A4 收口结果）。

## 六、统一右键菜单与多级菜单

当前自绘 PopupMenu、官方 Select、右键入口和编辑器菜单应共享同一套菜单模型与行为。

### 6.1 菜单模型

```cpp
struct AppMenuItem {
    MenuItemId id;
    std::string label;
    std::optional<ImageResource> icon;
    std::optional<std::string> shortcut;
    MenuTone tone;
    bool enabled;
    bool checked;
    std::vector<AppMenuItem> children;
};
```

统一支持普通项、选中项、分隔、危险项、禁用项和子菜单，渲染入口可以是右键、`⋮`、
工具栏菜单或键盘快捷键，但行为和视觉必须相同。

### 6.2 一级、二级、三级菜单行为

- 一级菜单锚定触发按钮或指针位置，并保证不越出窗口可视区域。
- 二级菜单在父项侧边展开；空间不足时自动向反方向展开。
- 二级菜单是完整支持目标；数据模型允许递归到三级，但业务 UI 不主动采用三级。
- 确实无法扁平化的三级菜单沿用同一方向判断；优先改用命令面板、Dialog 或独立设置页。
- 鼠标从父项移向子菜单时应有安全走廊/延迟关闭，不能因穿过间隙立即消失。
- hover 打开子菜单应有短延迟；点击带子菜单项也可以立即打开。
- `Esc` 每次关闭最深一层；连续按下逐层退出。
- 方向键支持上下移动、左右进入/退出子菜单，`Enter`/`Space` 激活。
- 点击菜单外关闭整棵菜单；窗口失焦、页面卸载或锚点消失也必须关闭。
- 菜单打开期间只有一条活动路径；切换同级项时关闭旧的下级分支。
- 危险项遵循现有 `PopupMenuDanger` 约定，并在最终破坏动作前统一进入危险确认。
- 菜单项高度、水平内边距、图标列、快捷键列和子菜单箭头列全局统一。

### 6.3 实现边界

- 先建立应用层菜单模型，再由统一渲染器适配 HuxerUI `UsePopup`/
  `ShowPopupMenuAt`；页面不能继续手写菜单布局。
- `Select` 仍用于表单选择，不与动作菜单合并概念；但其表面、圆角、item 高度、
  hover 和键盘行为应与菜单主题一致。
- 实施前确认 HuxerUI 当前版本对子菜单、焦点路由和 popup 嵌套的支持；缺失能力应形成
  独立上游能力计划，不能在页面内分别制造临时实现。

## 七、项目上下文与项目状态

当前全局底部条应按“配置上下文”和“实时状态”拆分，而不是整体搬成一个醒目的底部岛：

- 项目上下文条：当前环境、项目 Cookie、项目公共头/项目设置入口，靠近项目标题或
  请求工作区顶部。
- 项目状态条：请求运行、连接、响应时间、后台任务和错误摘要，保持低视觉存在感。
- 应用级配置：请求代理、默认超时和真正的全局公共头继续留在全局设置；若保留快捷
  入口，必须明确标注“应用代理”，不能伪装成项目属性。
- 保持全局或转为通知：应用更新、真正跨项目的后台任务、全局错误。
- 两者仅在打开项目时出现，主页和全局设置页不显示空条。
- 切换项目后全部内容必须来自活动项目上下文，不能残留上一个项目的摘要。
- Compact 模式只显示关键状态图标和异常数量，详细内容进入点击后的状态面板。
- 设置入口和实时状态不能混成同一组文字热区；状态项可作为入口，但必须使用统一的
  状态动作组件。

已完成初次作用域审计：`GlobalCookie` 带 `projectId` 且所有查询按项目过滤，移动并
改名无需领域变更；环境定义也是项目级，当前选择按项目缓存在内存中；请求代理来自
单一 `settings.ini` preference，并在所有请求 finalize 时读取，是真正应用级。只有
将代理产品语义改为项目级时才新增项目设置存储；若环境选择要跨会话恢复，也需要新增
持久化。未来多窗口还需解耦 `RequestStore` 的全局当前项目/环境游标，避免串窗。

## 八、响应式约定

- 宽屏：工具岛、主工作岛和辅助岛并排。
- 中等宽度：辅助岛折叠到主岛标签或可切换面板。
- Compact：项目工具岛切换为底部工具岛或临时抽屉；主编辑区优先获得宽度。
- 页面外间距可缩小，但按钮命中区、列表行高度和输入区内边距不随意压缩。
- Dialog 在 Compact 下可升级为接近全屏的 sheet。

## 九、自动优先级与实施顺序

优先级定义：P0 = 建立后续所有 UI 改造依赖的基础且风险可控；P1 = 关键体验收束；
P2 = 扩展能力、复杂交互或全量迁移。

1. **P0**：建立柔和冷中性的灰白浅色令牌、岛屿结构令牌和几何令牌。
2. **P0**：建立 `IslandSurface`、`IslandSection`、`IslandDialog`、`AppIconButton`。
3. **P0**：修复现有 `⋮`/尾部图标动作的命中区与语义标签，不改变业务流程。
4. **P0**：用全局设置页验证基础组件、深浅主题和 bounded/Compact 布局。
5. **P1**：改造应用壳的顶部导航与项目工具岛；删除不必要的内容层嵌套岛。
6. **P1**：按已审计作用域拆分项目上下文条与低存在感项目状态条；应用代理留在全局
   设置，`GlobalCookie` 在 UI 改名“项目 Cookie”。
7. **P1**：建立统一列表行和 `TrailingActionGroup`，迁移主页与历史页。
8. **P1**：建立应用菜单模型和一级/二级 Popup 渲染器，迁移右键与 `⋮` 菜单。
9. **P1**：建立 `AdaptiveInputSurface`，首批只用于真正的命令/搜索输入；请求 URL
   使用大型圆角组合栏。
10. **P2**：改造请求页，消除卡片嵌套并接入统一菜单、列表动作和项目上下文。
11. **P2**：迁移压测、Mock、测试用例、项目设置等其余页面。
12. **P2**：数据模型保留三级菜单能力；仅在真实需求无法扁平化时实现 UI。
13. **P2**：删除旧的手工岛面样式、重复图标按钮和页面私有菜单实现。
14. **P2**：完成深色/浅色、标准/Compact、鼠标/键盘四组视觉与交互回归。

## 十、验收标准

- 浅色模式没有默认大面积纯白岛面，整体呈柔和冷中性灰白且文字对比度清晰。
- 任一岛屿都能说明其业务边界；移除背景后，布局层级仍然可理解。
- 页面代码不再直接复制岛屿的背景、圆角、阴影和外层 Padding 组合。
- agent/命令类单行大输入是胶囊，多行自然拉伸且角部不变形、不跳动；请求 URL 保持
  高密度圆角组合栏。
- 同类列表尾部按钮尺寸、位置和命中区一致；`⋮` 的可点击区域明显大于字形。
- 所有纯图标小按钮共享统一尺寸体系，并按场景使用圆形、圆角方形或无底外观；均具有
  Tooltip、语义标签、焦点态和完整命中反馈。
- 右键、`⋮` 和工具栏动作菜单共享同一视觉与键盘模型；二、三级菜单可稳定进出。
- 项目上下文条与项目状态条只在项目上下文中出现，切换项目后内容同步切换。
- 同一屏幕不出现无意义的卡片套卡片，也不超过三级表面层级。

## 十一、实施前待确认事项

- 冷中性灰白最终色值与文字对比度，通过实际 Linux/Windows 显示器截图校准。
- 当前产品是否存在足够明确的 agent/命令输入场景；未出现前不为其提前制造页面结构。
- HuxerUI 对嵌套 popup、焦点捕获、子菜单安全走廊的现有能力边界。
- 项目上下文条的最终位置，以及环境选择是否需要跨会话持久化。
- Compact 下工具岛采用底部栏还是抽屉，待主页面线框验证后决定。

## 十二、下一执行批次：P1-A 圆角统一与胶囊特殊化

本节是下一组 agent 的直接施工单。P1-A 只统一几何、输入表面和列表动作，不同时重构
项目状态条或完整多级菜单，避免一次跨越过多业务边界。实现应基于当前 HuxerUI 0.2.0
源码/SDK 双通道；不得恢复旧版 SDK 兼容分支。

### 12.1 批次目标

- 建立唯一的圆角与控件尺寸来源，消除页面散落的魔法数字。
- 将胶囊限制为输入、短标签和少量独立动作的特殊造型，普通按钮保持圆角矩形。
- 统一列表尾部动作的占位、尺寸、对齐和事件隔离。
- 为后续菜单统一提供稳定的 `OverflowButton` 入口，但本批不实现递归菜单树。
- 保持现有业务流程、状态存储、快捷键和菜单内容不变。

### 12.2 工作包与优先级

#### P1-A1：几何令牌与基础按钮（阻塞其余工作包）

主要文件：`src/ui/ui.h`、`src/ui/common.cpp`，必要时 `src/ui/app.cpp`。

- 在 `IslandTheme` 或相邻主题结构中加入语义化几何令牌：
  `island_radius`、`nested_radius`、`control_radius`、`large_control_radius`、
  `icon_button_compact`、`icon_button_regular`、`control_height`。
- 提供按容器层级解析内层圆角的公共函数，优先保证父子表面近似同心；页面不自行计算。
- 收束 `AppIconButton`：字形固定 14–16pt，命中区固定 28/32pt，形状不再由文本尺寸推导。
- 增加 `TrailingActionGroup` 和 `OverflowButton` 的最小实现；前者固定动作槽宽度，后者
  只封装已有菜单触发回调，不引入新菜单数据模型。
- 搜索并迁移/删除仍存活的 `CircleButton` 或同义私有实现，最终只能保留一套图标按钮体系。

交付条件：公共组件能独立编译；深浅主题几何完全一致；任何尺寸变化不改变业务回调。

#### P1-A2：统一列表条与尾部动作

主要文件：`src/ui/home_page.cpp`、`src/ui/history_page.cpp`；只有公共 API 缺陷才回改
`ui.h/common.cpp`。

- 建立稳定的“前置区—主内容—尾部信息—固定动作区”布局配方。
- 首批迁移主页项目列表和历史列表，所有同类尾部按钮统一宽度、高度和图标尺寸。
- 点击尾部 `⋮`、删除或关闭不得冒泡触发行选择；hover/focus 覆盖整个按钮命中区。
- 无动作行是否占位必须按列表整体决定，禁止同一列表混用导致文字左右跳动。
- 保留当前菜单内容和确认流程，不在本工作包重写 Popup 行为。

交付条件：窗口宽度变化时右侧动作列不抖动；键盘聚焦顺序与视觉顺序一致。

#### P1-A3：大型输入表面与请求组合栏

主要文件：`src/ui/common.cpp`、`src/ui/request_page.cpp`；若当前没有真实 agent/命令输入，
只实现可复用原语，不虚构新入口。

- 实现 `AdaptiveInputSurface` 的最小可用 API：单行可用 full capsule，多行切换为固定大圆角；
  内边距、焦点边界和尾部动作位置在切换时保持稳定。
- 请求 URL 行使用 10–12pt 大圆角组合栏，不做 full capsule；方法、URL、环境和发送动作
  共享外轮廓，内部不重复描边。
- 设置正文区域的最小/最大高度；跨过最大高度后只滚动正文，不扩大整个页面。
- 错误、禁用、发送中只改变 tone/outline，不改变整体几何。

交付条件：单行/多行切换没有横向跳动；Compact 宽度下不遮挡发送/取消动作。

#### P1-A4：视觉审计与回归收口

主要范围：`src/ui/*.cpp` 只读审计；确有重复时提交小范围修正。

- 用 `rg` 列出散落的 `CornerRadius`、固定 `Width/Height` 和文本图标按钮，分类为：
  必须迁移、业务特例、暂缓到 P1-B/P2。
- 检查浅色米白、深色、标准宽度和 Compact 四种组合。
- 验证鼠标 hover/press、键盘 Tab/Enter/Space、Tooltip 和语义名称。
- 更新本文“已完成”列表，记录仍存在的特例及原因；禁止只改代码不回写计划状态。

交付条件：`git diff --check`、`cmake --build build --target apitab -j2`、
`ctest --test-dir build --output-on-failure` 全部通过。

#### P1-A4 收口结果（2026-09-01）

构建收口：`cmake --build build --target apitab -j2` 通过（仅第三方头
`-Wsubobject-linkage` 警告，无 error）；`ctest --test-dir build --output-on-failure`
1/1 通过；`git diff --check` 干净。

**交付差异与判定**

- `CircleButton` 已删除（声明 ui.h、定义 common.cpp；A2/A3 迁移后调用点归零，
  request_page 原注释同步改写）。全仓只剩 `AppIconButton` 一套图标按钮体系。
- **Tooltip 判定：已接入，非缺口**。SDK 的 `Tooltip`（presentation.h）是声明式
  modifier，`TooltipService` 在框架根自动装配（tooltip.cpp），`TooltipStyle` 由
  Material 主题按 `inverse_surface` 自动注入（theme.cpp），官方 gallery 即
  `IconButton(...).With(Tooltip(label))` 用法，本仓工具岛此前已直接使用。已在
  `AppIconButton` 单点接入：`semanticLabel` 复用为 Tooltip 文本，全部调用点
  （含 OverflowButton）零改动自动获得；禁用态仍可悬停查看（官方同款）。
- **顺带修正的集成遗漏（P1-A1 遗留）**：AppIconButton 非 Bare 形状原先显式传空
  `Indication{}`——按 SDK 修饰符替换规则（view.cpp `AddModifier`：显式 Indication
  擦除 OnClick 自动追加的 DefaultIndication），这等于关掉 hover/press 反馈，违反
  §5.3/验收标准"完整命中反馈"。现三种形状统一挂显式 indication：tint =
  `accent ? on_primary : on_surface`（6% hover / 12% press），按宿主 corner radii
  裁剪、覆盖整块命中区，深浅主题自动适配（主题默认 indication 按 primary 取色，
  在 accent 按钮的 primary 底上不可见，故不走默认）。**约定：空 `Indication{}`
  表示"无反馈"，永远不能用来表达"用默认"**（请求树/分组行整行的手工悬停底色
  配方属有意压制，不受影响）。

**散落审计三档清单（`rg` 全量，行号为收口时点）**

必须迁移（P1-B）：

1. `src/ui/app.cpp:349` — 顶部导航项目标签关闭 ✕ 仍是 `Text`+`OnClick`（命中区
   ≈字形+4pt padding、无语义标签/Tooltip）；随 §12.5 项 3"顶部导航岛与项目工具岛
   收束"迁 `AppIconButton(Bare, compact)` + 悬停显隐。
2. `src/ui/request_page.cpp:1011` — 环境配置 ☰ 触发器是 `Text`+`OnClick`（命中区
   不足 28pt、无语义标签/Tooltip）；随 P1-B 菜单统一迁 `AppIconButton(Bare)`，
   保持组合栏内扁平段外观。
3. `src/ui/request_page.cpp:2467、2529、2543、2566、2585、2597` — 请求页手工岛面
   （`Background(surface_container_low)+CornerRadius(shapes.large)+Padding` 复制
   组合，数值恰等于 island_radius=16）应换 `IslandSurface`；随 §九 项 10 请求页
   P2 改造执行，P1-B 触及请求页时可提前。

业务特例（保留，写明原因）：

1. `src/ui/request_page.cpp:937`、`src/ui/app.cpp:595` — 拖拽覆盖层克隆里的 ✕
   （纯展示、无事件，跟随被克隆行外观；P0 已定"拖拽预览符号保持纯展示"）。
2. `src/ui/request_page.cpp:2140、2209` — `CornerRadius(6.0F)` 拖影行圆角（纯展示
   克隆，视觉近似即可，不参与几何令牌体系）。
3. `src/ui/request_page.cpp:88`（KV 行类型 ▾）、`:977`（环境 ▾）、
   `src/ui/common.cpp:636`（方法 ▾）— 组合栏/表格行内的扁平下拉触发器（官方
   Select 触发器自带描边外观，塞不进共用外框，见 CLAUDE.md 口径）；属选择器
   体系而非图标按钮体系，随 P1-B 菜单统一核查 Select/菜单表面一致性。
4. `src/ui/request_page.cpp:2165` — 分组行折叠箭头 ▸/▾：点击目标是整行
   （分组行 Row OnClick），箭头只是状态指示器，不是文本按钮。
5. `src/ui/app.cpp:330` — 项目标签名称区 `Text(name)`：标签本体是文本型控件，
   点击区是整个切换区 Row，合规。
6. `src/ui/home_page.cpp:95` — `ProjectCard` 固定 200×96 卡：内容卡片设计尺寸
   （非图标按钮/岛面），随 P2 主页模板化统一。
7. KV 行 28×28 phantom 占位（`src/ui/request_page.cpp:198`、`mock_page.cpp:80`、
   `settings_page.cpp:127`、`project_settings_page.cpp:81`）— 与 AppIconButton
   compact 槽位对齐的对齐占位，语义合理；P1-B 可抽公共占位 helper 收编。
8. `src/ui/app.cpp:675` — 工具岛宽 `compact ? 44 : 56` 与令牌 `rail_width=48`
   不同源：这是响应式两档宽度，与 rail_width 的语义不同；P1-B 收束工具岛时
   一并对齐或令牌化，暂保留。

暂缓 P2（§12.4 本批明确不迁移的页面存量）：

1. 手工岛面（`Background+CornerRadius(shapes.large)+Padding` 组合）：
   `project_settings_page.cpp:194`、`loadtest_page.cpp:209`、
   `http_test_page.cpp:209`、`history_page.cpp:148`、`testcase_page.cpp:516`、
   `home_page.cpp:233/313`（Dialog 内输入卡）— 随各页 P2 迁移统一换
   `IslandSurface`（settings_page 已迁，是参照实现）。
2. 其余固定 `Frame` 布局尺寸（Dialog 宽 320/520/680×460、列表 max_width 截断列、
   loadtest 260/240 高区块、http_test 140/120 标签列等）— 属布局尺寸而非圆角/
   控件尺寸体系，随 P2 页面改造收编，不做提前机械替换。

**四组合静态核验（无 GUI，代码级；行号为收口时点）**

1. **浅色/深色几何一致性：通过**。`ResolveIslandTheme`（common.cpp:19-41）中全部
   几何令牌与主题无关：island_radius=16、nested_radius=8、28/32/32 为常量；
   control/large_control 取 `shapes.small/medium`，两套 Minimal spec 均派生自
   Material spec 且不改 shapes（ShapeScheme 默认 4/8/12/16/28/10000 恒定）；
   `IslandColor` 只按 level 取色。AppIconButton / AdaptiveInputSurface /
   MethodUrlBar 的几何输入只有令牌，颜色输入只有 `theme.colors.*` → 几何不随
   主题分支，颜色全部随主题。
2. **标准/Compact 布局契约：通过（代码级）**。请求页操作行（request_page.cpp:1421
   注释、1427 MethodUrlBar）中 MethodUrlBar 是唯一 Grow 子元素，发送/取消（1445）、
   保存（1540）、⋮（1584）均为自然宽度；MethodUrlBar 外框 `ClipChildren`
   （common.cpp:708）+ URL 字段 Grow + baseUrl 段 `max_width=160`+`ClipChildren`
   （common.cpp:686）→ 收缩序 = URL 字段先缩、baseUrl 段后裁，右侧动作不被
   挤压（A3 注释复核属实）。`AdaptiveInputSurface` 两态仅圆角不同，正文限高滚动
   （common.cpp:205-262）。history 行以 Grow 主内容把尾部信息推到行右缘、整列表
   统一无动作槽（history_page.cpp:23-52），宽度变化不抖动。
3. **hover/press 覆盖整个命中区：通过（含本批修正）**。三种 shape 共用同一显式
   indication（common.cpp:140-156），fill 按宿主 corner radii 裁剪 = 整块命中区；
   Bare 常态透明、同 tint。修正前非 Bare 因空 `Indication{}` 无反馈（见上）。
   请求树/分组行整行 OnClick 显式压默认 Indication、由手工底色承担整行悬停
   （request_page.cpp:2093-2104），无双层叠加。
4. **Semantics 语义标签：通过（抽查）**。AppIconButton 强制 semanticLabel 并挂
   `Semantics{Button, label}`（common.cpp:159-160）；调用点抽查均显式中文语义名：
   home_page"新建组织/新建项目/删除组织"、request_page"删除此行/重命名环境/
   删除环境/新建环境/关闭请求标签/新建请求标签/更多操作"、app.cpp"删除 Cookie"、
   mock/settings/project_settings"删除此行"、testcase"删除测试用例/删除断言"；
   OverflowButton 固定"更多操作"；TrailingActionGroup 只做排布、语义由内部按钮
   承担（home_page.cpp:47-60）。
5. **键盘 Tab 序 = 视觉序：通过（代码级）**。框架 `MoveFocus` 按
   `CollectFocusableNodes` 先序 DFS（runtime.cpp:1018-1025、2195-2228）= 声明
   顺序；home_page 组织/项目列子节点按视觉序声明（标题行 → 列表行内 文本→动作区，
   无打乱顺序的包装，home_page.cpp:34-60）；history 行无焦点件、不影响焦点序。

**仍需人工实机回归（agent 无法替代，不视为已完成）**

- 真实显示器上的浅色米白校准：ocean/base/raised/active/overlay 五档色阶观感与
  文字对比度（§十一 项 1）。
- Compact 实测：窄窗下请求页操作行收缩序、组合栏 baseUrl 裁剪、发送/保存/⋮
  不被遮挡；工具岛 compact 两档宽度。
- 鼠标全交互回归：AppIconButton 三形状 hover/press 观感（尤其 accent 按钮新的
  on_primary 叠加 tint 与深色主题 Bare 按钮）、Tooltip 出现/消失时序与遮挡、
  行选择与尾部动作点击隔离。
- 键盘全交互回归：Tab 序实机走查（home/history/请求页）、Enter/Space 激活、
  焦点环可见性、Semantics 屏幕阅读器朗读。

### 12.3 多 agent 并行方式

建议最多三个实现 agent 加一个收口 agent：

1. **基础组件 agent** 先独占 `src/ui/ui.h`、`src/ui/common.cpp` 完成 P1-A1，并提交/通知
   公共 API 已稳定。
2. **列表 agent** 在 A1 API 稳定后处理 `home_page.cpp/history_page.cpp`；不自行复制组件。
3. **输入 agent** 在 A1 API 稳定后处理 `request_page.cpp`；若需改公共 API，先与基础组件
   agent 协调，避免并发编辑 `common.cpp`。
4. **收口 agent** 最后执行 A4、解决集成冲突、运行全套构建测试并回写文档。

并行边界：A2 与 A3 可以并行；A1 必须先完成，A4 必须最后执行。同一时间只允许一个
agent 修改 `ui.h/common.cpp`。各 agent 不得顺手迁移职责外页面，也不得重排无关代码。

### 12.4 本批明确不做

- 不实现项目上下文条/项目状态条迁移，该项进入 P1-B。
- 不实现递归 `AppMenuItem`、二级/三级 Popup、安全走廊和键盘菜单树，该项进入 P1-B。
- 不全面迁移 Mock、测试用例、压测和项目设置页，该项仍为 P2。
- 不新增阴影系统、动画框架、自定义九宫格渲染或新的业务状态。
- 不把请求 URL 组合栏、普通确认按钮或所有标签强制改成胶囊。

### 12.5 P1-A 完成后的下一批 P1-B

P1-A 验收后再启动：

1. 应用菜单模型、统一一级/二级菜单渲染与键盘行为。
2. 项目上下文条和低存在感项目状态条，UI 中 `GlobalCookie` 政名“项目 Cookie”。
3. 顶部导航岛与项目工具岛收束，清理应用壳的重复表面。
4. 根据两个以上页面的真实重复决定是否抽取 `IslandPage`、`IslandToolbar`、
   `ProjectStatusDock`，不提前建立万能组件。

## 十三、下一执行批次：P1-B0 顶级标签与设置工作区统一

新增需求（2026-09-02）：点击“全局设置”后再点击当前已激活的项目，页面仍停留在设置。
根因不是单个点击回调，而是顶层位置由 `activeProject` 与 `navPage` 两个可互相矛盾的
状态共同表达：进入设置只改 `navPage`，项目仍保持 active；再次点同一项目时，现有
`activateProject` 只在 `navPage == kHome` 时切回请求页，因此没有退出设置。

本批不做条件补丁，而是把主页、项目和全局设置统一为同一套顶级标签行为。设置内部
再建立“通用 / 外观 / 关于”左侧分区导航。P1-B0 优先级高于原 P1-B 菜单和状态条，
因为后两者都依赖稳定的顶层工作区边界。

### 13.1 产品模型

顶级标签条采用三种身份：

```cpp
enum class TopTabKind {
    Home,
    Project,
    GlobalSettings,
};

struct TopTabId {
    TopTabKind kind;
    std::int64_t project_id = 0; // 仅 Project 使用
};
```

- **主页标签**：固定在最左，不可关闭、不参与拖拽。
- **项目标签**：沿用当前可打开、关闭、排序和持久化行为。
- **全局设置标签**：单例；点击右上设置按钮时“若未打开则加入，已打开则仅激活”，
  不允许重复出现多个设置标签。
- 设置标签作为真正的顶级标签参与 active 判定；点击任何项目标签都必须切回该项目
  工作区，即使它此前已经是 `activeProject`。
- 设置标签默认可关闭但不参与项目拖拽排序和 `open_projects` 持久化；关闭后回到最近
  激活的项目，若没有项目则回主页。
- 项目 `activeProject` 只表达领域层当前项目，不再兼任“当前顶级标签”。设置激活时
  不清空领域当前项目，避免返回项目后重新加载；视觉 active 由 `activeTopTab` 决定。
- `navPage` 只表达项目工作区内部页面（请求、压测、历史、项目设置等），不再包含
  `kHome` / `kAppSettings` 这种顶级目的地。若为降低首批改动风险暂不删除旧枚举，
  也必须由单一映射函数集中兼容，页面点击回调不得继续直接拼接两个状态。

`HttpTest` 当前由标题栏独立入口打开，语义上也是非项目工作区。P1-B0 首批可以保持
现状，但必须在代码中记录后续选择：要么升级为同类单例顶级标签，要么改为 Dialog/
工具窗口；禁止继续把它伪装成项目 `navPage`。本批不要顺手扩大到新标签类型。

### 13.2 顶级标签状态与回退规则

建议状态：

```text
open_projects       项目标签顺序，只存 project id
settings_open       设置单例标签是否存在
active_top_tab      Home / Project(id) / GlobalSettings
last_project_tab    最近激活且仍打开的 project id（用于关闭设置后回退）
project_page        项目工作区内部页
```

状态不变量：

1. `active_top_tab == Project(id)` 时，`id` 必须存在于 `open_projects`，并与领域当前项目
   同步；同步完成前不得渲染该项目页面。
2. `active_top_tab == GlobalSettings` 时，`settings_open == true`；设置页面整宽显示，
   不显示项目工具岛、项目上下文条或项目状态条。
3. 点击项目标签总是同时激活顶级标签和领域项目；不能因为 project id 未变化而跳过
   顶级切换。
4. 关闭活动项目时优先激活相邻项目；无项目后回主页。关闭非活动项目不改变当前顶级页。
5. 关闭设置时优先回 `last_project_tab`；该项目已关闭则回当前项目列表中的相邻/最后
   一项；仍无项目则回主页。
6. 设置页内部分类切换不创建新顶级标签，也不改变设置标签标题。
7. 主题切换造成根树重组时必须保持设置标签与当前分类，不得跳回主页或通用分区。

### 13.3 设置工作区信息架构

设置标签标题统一为“设置”，页面标题可显示“全局设置”。设置页面采用“左侧分类岛 +
右侧内容岛”，不把每个设置项继续套成独立卡片：

```text
设置顶级标签
└── 设置工作区
    ├── 左侧分类岛（固定/有界宽度）
    │   ├── 通用
    │   ├── 外观
    │   └── 关于
    └── 右侧设置内容岛（独立滚动）
```

分类归属：

- **通用**：关闭行为、默认请求超时、应用级全局公共请求头、数据目录；后续应用代理
  正式迁入这里。文案必须明确“应用级”，不能与项目 Cookie/环境混淆。
- **外观**：主题模式及未来界面密度、动效偏好；现有圆形主题揭示动画保留，切换后
  仍停留在外观分类。
- **关于**：应用名称与版本、HuxerUI 版本、构建/许可证入口、数据与文档位置；首批
  没有可靠运行时版本 API 时可使用构建期常量，但禁止硬编码无法同步的假版本。

左侧分类行为：

- 分类项使用统一选中/hover/focus 表面，整行可点，有可访问名称；键盘上下移动、
  Enter/Space 激活，Tab 顺序先分类后内容。
- 标准宽度保持左栏约 176–208pt；右侧内容独立滚动，左栏不随长表单滚走。
- Compact 下左栏折叠为顶部分类选择器或可横向滚动的分段条；本批优先顶部选择器，
  不引入 Drawer。分类切换不得重置各分区尚未提交的受控输入状态。
- 设置分区状态由 `GlobalSettingsPage` 自身稳定 State 持有；顶级设置标签关闭后允许销毁，
  再打开默认进入上次会话内分类或“通用”，二者任选其一但必须在实现中固定并测试。

### 13.4 工作包与依赖顺序

#### P1-B0.1：顶级标签状态模型（先行、阻塞其他工作包）

主要文件：`src/ui/app.cpp`、`src/ui/ui.h`。

- 引入明确的 `TopTabId/TopTabKind` 或等价强类型，不允许用负数 project id 充当设置。
- 改造 `ProjectTab`/`ProjectTabStrip` 为通用顶级标签条，项目拖拽 payload 仍只接受项目。
- 设置按钮改为打开/激活设置单例标签；项目点击无条件切换 `active_top_tab`。
- 集中实现 `ActivateTopTab`、`CloseTopTab` 与回退选择，替代散落的 `navPage` 写入。
- 保留 `open_projects` 数据格式，设置标签不写入该字段；避免无关的数据迁移。
- 用纯状态函数或最小可测试 helper 覆盖打开、重复打开、切换和关闭回退矩阵。

#### P1-B0.2：设置左侧分区与内容迁移

主要文件：`src/ui/settings_page.cpp`，公共组件确有两个以上调用场景后才修改
`src/ui/common.cpp/ui.h`。

- 将当前连续长表单拆成 `GeneralSettingsSection`、`AppearanceSettingsSection`、
  `AboutSettingsSection`（命名可调整，职责不可混合）。
- 外层采用双岛布局；设置内容区独立滚动，左侧分类保持固定。
- 保留现有 preferences key、主题过渡、全局头序列化和发送侧语义，禁止借 UI 重排
  修改配置存储格式。
- 修正文案：项目 Cookie 不再称“全局 Cookie”；底部快捷入口的过渡文案应与 P1-B1
  状态条计划一致。
- About 首批只展示可验证信息和入口，不制造在线更新、遥测或账户能力。

#### P1-B0.3：P1-A 验收缺口顺带收口

主要文件：`src/ui/app.cpp`、`src/ui/request_page.cpp`。

- 项目标签真实关闭入口从 `Text("✕") + OnClick` 迁为 Bare compact
  `AppIconButton`；保持悬停显隐和固定占位。拖拽预览中的 `✕` 仍为纯展示。
- 环境配置 `☰` 从文本按钮迁为 Bare `AppIconButton`，保持组合栏扁平外观和原 Dialog。
- 运行 P1-A 尚未完成的浅/深、标准/Compact、鼠标与键盘实机回归，并把结论回写本文。

#### P1-B0.4：集成验收与文档收口（最后执行）

- 删除或隔离顶层用途的 `kHome/kAppSettings` 分支，确认不存在双状态竞争路径。
- 全量搜索直接写 `navPage = kAppSettings/kHome` 的调用点并归零或集中到兼容层。
- 更新本文状态：只有行为矩阵与实机回归通过后，才把 P1-A 和 P1-B0 标为完成。
- 运行 `git diff --check`、`cmake --build build --target apitab -j2`、
  `ctest --test-dir build --output-on-failure`。

### 13.5 多 agent 执行边界

推荐三阶段而不是同时修改应用壳：

1. **Shell agent** 独占 `app.cpp/ui.h` 完成 B0.1；这是唯一允许改顶级状态模型的 agent。
2. B0.1 API 稳定后，**Settings agent** 独占 `settings_page.cpp` 完成 B0.2；同时
   **Controls agent** 只处理 B0.3 的 `request_page.cpp` 环境按钮。项目标签关闭按钮由
   Shell agent 一并处理，避免第二个 agent 并发修改 `app.cpp`。
3. **QA agent** 最后完成 B0.4、行为矩阵、实机回归和文档状态更新，不新增产品功能。

`common.cpp/ui.h` 若需新增设置分类公共组件，必须先由 Settings agent 提出真实重复证据，
再交 Shell agent 或集成 agent 串行修改。同一时刻不得有两个 agent 编辑 `app.cpp`、
`ui.h` 或 `settings_page.cpp`。

### 13.6 验收行为矩阵

至少逐项验证：

| 初始位置 | 操作 | 期望结果 |
|---|---|---|
| 项目 A | 点击设置 | 打开并激活唯一设置标签，A 保持打开 |
| 设置 | 再点设置按钮 | 不新增标签，仍停留设置 |
| 设置 | 点击项目 A | 立即显示 A 的原项目内部页面 |
| 设置 | 点击项目 B | 激活 B、同步领域当前项目并显示 B 工作区 |
| 设置 | 关闭设置标签 | 回最近仍打开的项目；无项目则主页 |
| 项目 A | 关闭非活动设置标签 | A 保持活动，页面不闪回 |
| 设置/外观 | 切换主题 | 设置标签仍活动，分类仍为外观 |
| 设置/通用 | 编辑未失焦输入后切外观再切回 | 受控值不丢失、不重复写坏配置 |
| 项目 A | 关闭 A | 激活相邻项目；无项目则主页 |
| 主页 | 点击设置再关闭 | 回主页 |

同时验证：设置标签不会进入项目拖拽排序或 `open_projects`；设置活动时项目侧栏/项目状态
不显示；窗口重绘和主题切换没有短暂渲染旧项目内容；键盘可完成顶级标签切换、分类导航、
关闭设置和返回项目。

#### P1-B0.4 收口结果（2026-09-02）

构建收口：`cmake --build build --target apitab -j2` 通过（仅第三方头
`-Wsubobject-linkage` 警告，无 error）；`ctest --test-dir build --output-on-failure`
1/1 通过；`git diff --check` 干净；`./build/apitab --cli help` 冒烟正常（CLI 未受
B0 改动影响）。本节结论除特别注明外均为**代码级静态核验**；实机项见文末，一律
"待人工"，不标已完成。

**双状态竞争归零证据**（`rg -n "navPage" src/`、
`rg -n "kHome|kAppSettings|activeProject =|activeTopTab" src/ui/` 全量核对；行号为
收口时点）：

- `navPage` 全部写入点 5 处，无一处写顶级目的地：
  - `src/ui/app.cpp:1192`（初值 kRequest）；
  - `src/ui/app.cpp:727`（SideShell 侧栏切页，目标 ∈ kRequest/kLoad/kHistory/
    kProjectSettings，见 items 表 app.cpp:709-714）；
  - `src/ui/app.cpp:1264`、`:1288`（任何顶级激活/活动顶级标签变化后退出遗留
    HttpTest → kRequest）；
  - `src/ui/app.cpp:1434`（闪电按钮 → kHttpTest，遗留全宽入口，§13.1 记录在案、
    P1-B1 项 4 评估）。
- `kHome/kAppSettings` 已从 PageIndex 删除（app.cpp:44-50 仅剩 kRequest/kLoad/
  kHistory/kProjectSettings/kHttpTest），`PageFor`（app.cpp:52-68）无顶级目的地
  分支。
- 顶级切换集中入口：标签激活/关闭统一走 AppRoot 注入的 actions（app.cpp:315/369）
  → `activateTopTab`/`closeTopTab` 推迟包装（app.cpp:1294-1306）→
  `activateTopTabNow`（app.cpp:1248）调纯 helper `ActivateHome/ActivateProject/
  ActivateSettings`（ui.h:194-217）、`closeTopTabNow`（app.cpp:1274）调
  `CloseTopTab`（ui.h:228-267）→ `commitTopTab` 统一写回（app.cpp:1231-1244）。
  齿轮按钮（app.cpp:1460）与 HomePage `onOpenProject`（app.cpp:1311-1313）
  汇入同一入口。
- `activeTopTab` 全仓唯一写入点 = `commitTopTab`（app.cpp:1242）；`settingsOpen`
  /`lastProjectTab` 同样只经 commitTopTab（app.cpp:1241/1243）；`activeProject`
  仅 syncDomainProject（app.cpp:1225）与活动项目关闭清零（app.cpp:1283）。拖拽
  换位只重排 tabs 并持久化（app.cpp:468-474），不触碰顶级状态。

**§13.6 行为矩阵逐行核验（静态；"通过" = 代码级核验通过，实机走查仍待人工）**

| 初始位置 | 操作 | 结论 | 证据（file:line 为收口时点） |
|---|---|---|---|
| 项目 A | 点击设置 | 通过 | 齿轮 → ActivateSettings（ui.h:213-217）：settings_open=true、active=GlobalSettings，open_projects/last_project 不动 → A 保持打开；标签条按 settingsOpen 布尔渲染至多一个设置标签（app.cpp:582-585），不重复 |
| 设置 | 再点设置按钮 | 通过 | ActivateSettings 幂等（重复赋值同字段、无列表追加，ui.h:213-217）；点设置标签本体同路径 |
| 设置 | 点击项目 A | 通过（附实际行为说明） | ActivateProject 只改 active/last_project（ui.h:202-209），navPage 不被设置流程触碰；内容 Key = "project:<id>:<navPage>"（app.cpp:1378-1379）→ 回 A 显示原内部页。**如实记录**：设置↔项目往返两端 Key 必然不同（"settings" ↔ "project:…"）→ 内容子树重挂载（keyed 复用只发生在同一父节点连续挂载的兄弟之间，runtime.cpp ReconcileChildren）；RequestPage 的 openDrafts 是页面 State（request_page.cpp:2486，从列表打开请求时填充 request_page.cpp:2120）而非 store/领域，往返后打开的请求标签与未保存草稿不保留；历史/压测/项目设置等组合期读 store 的内部页数据完好。该重挂载语义与 B0 之前一致（旧模型 navPage 切换同样卸载），非本批回归 |
| 设置 | 点击项目 B | 通过 | ActivateProject(B) + syncDomainProject（app.cpp:1265-1267）：selectProject/setProject/active_project 持久化/activeProject State 先于 commitTopTab 完成（不变量 1）→ 显示 B 工作区（侧栏 + PageFor，app.cpp:1377-1380） |
| 设置 | 关闭设置标签 | 通过 | CloseTopTab GlobalSettings（ui.h:250-264）：last_project 仍在打开列表 → 激活之；否则 open_projects 末项；仍无 → 主页；回退到项目则 closeTopTabNow 同步领域 |
| 项目 A | 关闭非活动设置标签 | 通过（前提修正） | 单例下设置标签可"打开但非活动"（ActivateProject 保持 settings_open，ui.h:200 注释"设置标签留在后台"）——矩阵行前提"要么活动要么不打开"不成立，但 helper 按防御处理：仅置 settings_open=false、active/last_project 不动（ui.h:250-252）→ A 保持活动，内容 Key 不变不重挂载、无闪回 |
| 设置/外观 | 切换主题 | 通过 | themeMode 写入（settings_page.cpp:476-477，经圆形过渡 mutation settings_page.cpp:490）只触发根重组；内容 Key 不含主题（app.cpp:1382-1384）→ activeTopTab 与分类 State（页顶层 settings_page.cpp:463）存活 |
| 设置/通用 | 编辑未失焦输入后切外观再切回 | 通过 | timeoutSec/globalHeaders/category 全部 UseState 挂 GlobalSettingsPage 顶层（settings_page.cpp:453-463），分类切换只改 category、分区重挂时从同一 State 取值 → 受控值不丢；OnChanged 即写 ini、切分类不产生额外写入 |
| 项目 A | 关闭 A | 通过 | CloseTopTab Project（ui.h:232-249）：右邻（删除后原下标元素）→ 左邻 → 主页；last_project 指向被关项目时随活动项目同步；活动项目被关且回主页时领域清零（selectProject(0)/setProject(0)/active_project="0"/activeProject=0，app.cpp:1279-1283）；回退到项目则 syncDomainProject |
| 主页 | 点击设置再关闭 | 通过 | 主页路径 last_project 恒 0（ActivateHome 不写，ui.h:194-197；ActivateSettings 不动它）→ 关设置无项目回主页（ui.h:253-262），last_project 保持 0 |

附加项：

- 设置标签不进拖拽、不进 `open_projects`：设置/主页标签 draggable=false
  （app.cpp:585、663-665），DragSource 只挂项目标签（app.cpp:416-420）；
  kSettingsTabDisplayKey 仅条内显示 key（app.cpp:82-84）；commitTopTab 只持久化
  open_projects（项目 id CSV，app.cpp:1232-1239），settingsOpen/activeTopTab/
  lastProjectTab 不持久化（app.cpp:1229-1230 注释）。
- 设置活动时项目侧栏不显示：showSideShell 仅 Project 分支置 true（app.cpp:1377），
  主行侧栏占位为空 Row（app.cpp:1475-1476）。**底部全局状态条仍全页显示**
  （app.cpp:1485，在页面分支之外）——与 §13.2 不变量 2 的最终形态有差距，属
  P1-B1 状态条拆分范围（§13.7 项 2），如实保留待改。
- 主题切换无短暂旧项目内容：领域写入先于 activeTopTab 同任务完成（app.cpp:1203-
  1205、1265-1267）+ 内容 Key 不含主题 → 重组无旧内容中间帧（代码级通过；真实
  观感待实机）。
- 关闭活动项目的回退矩阵与 §13.2 不变量逐条对齐：不变量 1（领域先行 + active 项目
  不在 tabs 时防御回落主页，app.cpp:1357-1359、1368-1370）、2（设置整宽、无项目
  岛，app.cpp:1372-1375）、3（点项目标签无条件激活顶级 + 领域，app.cpp:1217-1219、
  1265-1267）、4（右邻→左邻→主页；关非活动不动，ui.h:238-247）、5（last_project
  → 末项 → 主页，ui.h:253-263；回退按 open_projects 成员判断，覆盖"设置活动期间
  关闭后台项目致 last_project 失效"路径）、6（分类切换只写 category State，
  settings_page.cpp:217-224）、7（见主题行）。last_project 与活动项目的一致性由
  ActivateProject 每次激活同步维护（ui.h:207）。

**键盘可达性（修复与缺口）**

机制（SDK 源码核实）：内置 Button/IconButton/Chip 等 spec 自带 focusable=true +
activation（view.cpp MakeButtonSpec/MakeIconButtonSpec/MakeChipSpec）；自定义
Row+OnClick 默认不可聚焦，补 `Focusable(true)` 后进入 Tab 序（runtime.cpp
CollectFocusableNodes 收集 `enabled && focusable`），Enter/Space 经 ActivateNode 转
ViewEvents::Click（runtime.cpp 键盘激活路径）；disabled 节点不参与遍历。

本批修复（Focusable/Semantics 最小补丁，模式照搬 B0.2 左分类行
settings_page.cpp:213-216）：

1. 顶级标签切换区（app.cpp:328、347-349）：Focusable(true) +
   Semantics{Button, "主页"/项目名/"设置"}——键盘可完成顶级标签切换。
2. 齿轮（app.cpp:1453-1455）：Focusable + Semantics"全局设置"——键盘可打开/激活
   设置单例标签。
3. 闪电（app.cpp:1427-1429）：Focusable + Semantics"HTTP 协程压测"。
4. AppIconButton 组件本体（common.cpp:170）：统一补 Focusable(true)——补齐 §十
   "图标按钮…均具有…焦点态" 契约的最后一项，enabled 的图标动作（删除组织/删除行/
   环境 ☰/⋮ 等）键盘可达。
5. HomePage 项目卡片（home_page.cpp:108-110）：Focusable + Semantics
   "打开项目 <名>"——键盘可经主页打开/返回项目。

已有能力（无需改）：侧栏 IconButton、官方 Select/TextField/Button/Checkbox、Compact
分类 SegmentedButton 均为内置 focusable；设置左分类行 B0.2 已做。

缺口（如实记录）：

1. **顶级标签 ✕ 键盘不可达**（§13.6 键盘要求中唯一未闭合项）：enabled=hovered
   门控使未悬停的 ✕ 为 disabled、不参与 Tab 遍历 → "键盘关闭设置/项目标签"暂只能
   鼠标完成；已在 app.cpp:359-363 注释记录。改门控会变更指针行为（透明占位的误触
   防护），候选方案（hover∪focus 门控需组件暴露焦点态、或键盘快捷键）留 P1-B1
   顶部导航岛收束时统一决策。
2. 标准宽度设置左栏方向键导航是 SDK 能力缺口（runtime 键盘路径只处理 Tab/
   Shift+Tab），B0.2 已记录（settings_page.cpp:186-192）；Compact 分段条方向键由
   SegmentedButton 自带。
3. 底部状态条文字热区（StatusActionText，app.cpp:780）与主页组织行等仍为
   纯指针目标——不在本批修复清单（§13.6 键盘句未点名），随 P1-B1 状态条拆分与
   后续键盘专项处理。

**顺带发现（本批不修，记录给 P1-B1 / 独立决策）**

1. `open_projects` 与 `active_project` 从 GUI 侧只写不读：GUI 启动不恢复标签
   （AppRoot 初值 tabs={}、activeTopTab=Home，app.cpp:1193-1196；全仓无
   `sessionPreference("open_projects")` 读点）也不恢复 active_project（仅 CLI 读，
   src/cli.cpp:194）。docs/apitab-cli.md:55-56（"`--project ID`：……下次 GUI 启动
   会打开到该项目标签"）与 src/cli.cpp:10 的描述与现状不符——要么实现启动恢复、
   要么修正文档，留给 P1-B1 / 独立决策。
2. 遗留 HttpTest 顶级身份的 P1-B1 项 4 决策记录已在代码注释：src/ui/app.cpp:1407-
  1410（闪电按钮注释：升级单例顶级标签或改 Dialog/工具窗口，待 P1-B1 项 4 评估；
  禁止继续新增把它当项目页面 navPage 的入口），同见 app.cpp:61-62（PageFor）与
  app.cpp:1354-1356（内容区）。

**仍需人工实机回归（agent 无法替代，不视为已完成）**

- §13.6 行为矩阵全行的真实交互走查（上表结论均为代码级静态核验）。
- 真实显示器外观：米白/极简黑白主题下新设置三分区、顶级标签条的观感与对比度。
- Compact 实测：设置页顶部分段条分类切换、请求页操作行收缩、窄窗标签条滚动。
- 鼠标全交互：设置↔项目↔主页切换、标签 ✕ 悬停显隐、环境 ☰ Dialog 流程、主题
  圆形揭示过渡观感。
- 键盘全交互：Tab 序实机走查（含本批新增 Focusable 节点）、Enter/Space 激活、
  焦点环可见性（自定义 View 补 Focusable 后是否有可见焦点指示）。
- 窗口重绘/主题切换无短暂渲染旧项目内容（代码级已保证 Key 策略与领域先行，实机
  确认无闪帧）。

### 13.7 后续 P1-B1（P1-B0 验收后启动）

原 P1-B 顺延并改名 P1-B1：

1. 应用菜单模型、统一一级/二级菜单渲染与键盘行为。
2. 项目上下文条与低存在感项目状态条，UI 中 `GlobalCookie` 改名“项目 Cookie”。
3. 顶部导航岛和项目工具岛视觉收束；结构以 P1-B0 的统一顶级标签模型为准。
4. 评估 `HttpTest` 的顶级身份，并决定单例顶级标签或工具窗口/Dialog。
5. 根据两个以上页面的真实重复决定是否抽取 `IslandPage`、`IslandToolbar`、
   `ProjectStatusDock`。

## 十四、框架更新、设置间距与下一轮收口（2026-09-02）

### 14.1 已执行

- HuxerUI 源码从 `65ea1e6` 快进到 `937efb1 feat(combo-box): add editable suggestions`。
- 上游 `dcd41c4` 已正式修复 Linux/X11 `None` 宏冲突，对应本地补丁退役；只保留
  codegen/resource compiler 的动态 libstdc++ 两处本地 CMake 补丁。
- 按更新规则重建并安装当前源码的 Linux `hcg/hrc` 宿主工具。
- 重新检索多窗口/跨窗口拖放：937efb1 仍不提供，tear-off 计划继续阻塞。
- 全局设置标准宽度下的左右岛间距从页面级 `page_gap`（12pt）收紧为同工作区
  `theme.spacing.small`（8pt）。两个岛仍有独立边界，只减少中间空带；Compact 单岛
  不受影响。

### 14.2 ComboBox 能力边界

新 `huxerui::ComboBox` 是受控的单行可编辑建议框，不是 `Select` 的视觉别名：

- 应用持有完整 `TextEditingValue` 和候选集合。
- 直接输入触发 `OnChanged`；接受候选只触发 `OnSelected(index, value)`，不会再触发
  `OnChanged`，调用方必须在两条路径都回写受控值。
- 支持任意文本，不存在受控 selected index；过滤、排序、异步加载和缓存属于应用层。
- 键盘焦点留在输入框，Up/Down 选择候选、Enter 接受、Escape 关闭；IME 组合优先。
- 候选行不能包含独立可点击或可聚焦的子控件；稳定变化的候选应给 View `.Key(...)`。
- `ComboBoxStyle` 只管理候选浮层，输入本体继续使用 `TextFieldStyle`。

当前源码 main 已具备该 API，但仓库和 `~/.local/share/HuxerUI` 的 0.2.0 预编译 SDK
尚不包含它。开发期允许在默认源码通道直接使用 `ComboBox`，不再让旧 SDK 阻塞追新；
但进入合并/发布验收前必须刷新可消费 SDK，确保 `APITAB_HUXERUI_FORCE_SDK=ON` 恢复
通过。若某批必须在 SDK 刷新前临时保持旧通道可编译，只能使用明确的编译期能力检测，
不能长期维护两套业务交互实现。

### 14.3 应用场景审计

适合首批接入（P1-B1，按价值排序）：

1. **k6 时长**（`loadtest_page.cpp`）：允许 `250ms/10s/1m/1h` 等自由输入，同时提供
   `10s / 30s / 1m / 5m` 常用候选；这是最小、最清晰的首个验证点。
2. **Mock 状态码与测试用例期望状态码**（`mock_page.cpp`、`testcase_page.cpp`）：
   允许自定义状态码/表达式，候选展示常见 `200/201/204/400/401/403/404/500/503`；
   两处复用同一候选常量，不能各自维护列表。
3. **请求头名称**（请求、Mock、测试断言、全局头/项目头表）：允许任意头名，候选可
   提供 `Accept`、`Authorization`、`Content-Type`、`Cookie` 等；只有抽出共享候选与
   行组件后才迁移，避免五套表格分别复制 ComboBox 回调。
4. **代理地址**：只有引入“最近使用代理”或明确的 scheme 候选后才有价值；仅显示
   `http://`/`socks5://` 两个残缺字符串不值得替换。
5. **URL/环境 base URL/分组路径**：未来可以从请求历史、环境或已有分组生成候选，
   当前缺少去重和作用域规则，暂不实施。

不适合 ComboBox：

- 主题模式、关闭行为、分页条数、HTTP 方法、环境选择、KV 类型等封闭枚举继续用
  `Select`/分段控件/现有组合栏；允许任意输入会制造无效状态。
- VUs、超时、延迟等纯数值字段若没有真实高频候选，继续用 TextField；未来更适合
  数字输入/Stepper，而非为了使用新组件强行添加建议。
- 请求名称、组织/项目名称、变量值、消息正文等自由文本没有候选来源，不使用。

### 14.4 下一轮执行顺序

#### P1-B0.5：顶级标签可靠性收口（先于菜单与 ComboBox）

1. **标签状态保活**：设置标签仍打开但切到项目后，再切回必须保留原设置分类；查看
   设置不得丢失 RequestPage 的打开请求标签或未保存草稿。优先建立按 `TopTabId` /
   project id 保存的会话状态，或确认 HuxerUI 可用的 keep-alive 容器；禁止只保留
   设置分类、继续让未保存请求静默丢失。
2. **启动恢复**：读取 `open_projects` 与 `active_project`，解析/去重、过滤已删除或
   不可访问项目，恢复有效标签顺序和活动项目；数据无效时安全回主页。实现恢复，
   不删除 CLI 文档已有承诺。
3. **键盘关闭标签**：关闭按钮始终 enabled/focusable，只用 hover 或 focus 控制可见；
   或提供平台一致且可发现的关闭快捷键。焦点到达关闭按钮时必须显示，透明状态不能
   造成误触。
4. 文档把“键盘关闭是唯一未闭合项”改为“顶级标签行为矩阵中的未闭合项”；设置左栏
   方向键和底部状态条键盘可达性仍是独立缺口。
5. 实机完成设置↔项目状态保留、重启恢复、Compact、浅/深主题、鼠标/键盘矩阵。

收口约束：`IndexedPages` 内部页面的 `Grow` 不能代替其外层在主 Row 中的弹性语义；
主 Row 的 page 根必须显式 `Grow(1)`，否则请求页会按固有宽度测量，右岛越过窗口边界。
该回归已于 2026-09-02 修复并实机复核。

#### P1-B1.0：确立 SDK 驱动、源码优先的构建选择

- 保持 `huxerui build/run/package`、plan-only 自省、库图、hcg/hrc、资源合并和打包等
  SDK 工程契约；“源码优先”只改变框架实现来源，不另造一套非 CLI 构建流程。
- CMake 解析顺序固定为：显式 `HUXERUI_HOME` 源码 → `third_party/huxerui` 源码 →
  已安装 SDK → 仓库离线 SDK。CLI 自动注入的 SDK 前缀不得越过可用的仓库源码；
  `APITAB_HUXERUI_FORCE_SDK=ON` 是唯一明确跳过源码的开关。
- `huxerui build linux` 与 `huxerui run linux` 默认输出必须打印实际来源和源码 commit，
  使开发者能确认本次确实编译了最新 checkout；源码缺系统依赖而回落 SDK 时必须给出
  醒目原因，不能静默回落。
- 增加 CMake 选择矩阵测试，至少覆盖 CLI 注入 SDK + 仓库源码并存、显式外部源码、
  强制 SDK、源码缺失、源码依赖缺失五种情况。

#### P1-B1.0a：同步可消费 SDK（发布门禁）

- 从包含 937efb1 的源码构建并安装/归档 SDK，确认版本标识策略，更新 Linux tarball、
  SHA256、CMake 最低/精确消费规则与 `~/.local/bin/huxerui`。
- 分别完成源码模式、已安装 SDK 模式、仓库离线 SDK 模式构建；三者公开头必须一致。
- 更新 `.claude/skills/huxerui-app-development` 的上游 references，同时保留三个
  apitab local addendum。
- 开发批次以源码模式通过为日常门禁；准备合并、交付或更新离线环境时，B1.0a 三通道
  必须全部通过。SDK 同步可以落后于源码开发批次，但不得落后于对应发布点。

#### P1-B1.1：ComboBox 首批接入

- 只迁移 k6 时长作为组件验证点；保留任意合法输入和现有存储格式。
- 覆盖键盘、鼠标、IME、空候选、自由输入、接受候选以及 Compact 浮层定位。
- 源码通道完成组件验证后即可继续开发；扩展到状态码和共享请求头名称前完成 B1.0a，
  避免在 SDK 尚未同步时扩大新 API 使用面。
- 不把 ComboBox 接入与菜单模型重构放在同一个 agent/提交中。

#### P1-B1.2：原 P1-B1 继续

- 应用菜单模型与一级/二级菜单。
- 项目上下文条/状态条拆分，`GlobalCookie` 改名“项目 Cookie”。
- 顶部导航岛/项目工具岛收束，以及 HttpTest 顶级身份决策。
- 状态条拆分必须是对现有 `GlobalStatusBar` 的**替换式重构**，不能在标题栏下新增
  `ProjectContextBar`、同时在底部叠加 `ProjectStatusBar + GlobalStatusBar`。该提前实现
  曾造成项目页顶部多一层、底部多一层，已于 2026-09-02 回退。再次实施前先确定唯一
  信息架构：项目/环境/Cookie 不重复显示，全窗口纵向最多保留一条底部状态栏。

## 十五、自定义标题栏拖动与窗口缩放命中区（2026-09-02）

这不是岛屿视觉问题，而是自定义窗口装饰的命中区域没有完整建模，应作为
**P1-B0.6** 独立收口。当前现象为：左上角 Logo 所在区域可以拖动窗口，但标签条的
空白区域，以及标签与右侧闪电/设置按钮之间的大块弹性空白不能拖动；Linux 四边和
四角的缩放命中范围也明显偏窄。前者属于应用标题栏布局，后者至少部分属于 HuxerUI
平台适配层，两者不得用一个覆盖全标题栏的透明 View 临时修补。

### 15.1 标题栏命中区规则

标题栏必须显式划分为以下互斥区域，而不是依赖 `WindowTitleBar` 背板恰好透传：

1. **拖动区**：没有产品点击行为的 Logo 区、标签条自身未被标签占用的空白、标签条
   与右侧操作按钮之间的弹性留白。中间留白应使用独立的 `Grow(1)` spacer，并显式
   附加 `WindowDragRegion`；不能让一个可命中的普通 `Row` 吞掉整块空间。
2. **交互区**：主页/项目/设置标签本体、标签关闭按钮、闪电按钮、设置按钮以及系统
   窗口按钮。点击这些区域不得触发拖动，拖动判定也不得覆盖其 hover、按下、焦点和
   键盘行为。
3. **Logo 语义**：当前 Logo 如果没有实际点击命令，就按拖动区域表达，不伪装成按钮；
   若未来增加主页/菜单动作，必须再设计点击与拖动阈值，不能让同一简单点击同时具有
   两种含义。
4. **窗口行为**：标题栏空白区按平台习惯支持拖动；框架若已提供原生双击最大化/还原，
   显式拖动区不得破坏该行为。最大化、全屏或不可调整大小时，不得残留缩放命中。

应用侧优先在 `src/ui/app.cpp` 调整 `WindowTitleBar` 的子项结构：保留真实标签的交互
命中，只给明确的空白子项添加拖动语义。不要在顶层给包含全部标签与按钮的祖先统一
添加拖动语义，也不要通过降低按钮 z-order 来换取拖动。

### 15.2 缩放命中范围与框架边界

已定位 Linux 适配层使用固定的 `kResizeBorderDips = 6.0F`。6 DIP 在高分屏、触控板或
没有可见系统边框的自定义装饰窗口上容错不足。首选在 HuxerUI 提供可测试的窗口级策略，
而不是在 apitab 内容层模拟缩放：

- 建议默认有效命中宽度为四边 **10 DIP**、四角 **14 DIP**；这只扩大不可见热区，
  不增加可见边框，也不改变岛屿内边距。
- 若增加公开配置，采用类似 `WindowOptions::resize_border_thickness` 的逻辑 DIP 参数，
  并给出平台默认值、最小值和 DPI 换算规则。公共 API 一旦变化，开发期先由源码通道
  消费，并纳入 P1-B1.0a 的 SDK 重建与源码/安装 SDK/离线 SDK 三通道发布验证。
- Linux 使用逻辑 DIP 做边/角判定；Windows 至少取原生系统 resize metric 与配置值的
  较大者，避免缩小系统命中区；macOS 优先保留原生 resize 行为，只有复现同类问题后
  才覆盖平台策略。
- 缩放命中优先级高于标题栏拖动和内容命中；四角优先于单边。进入对应区域时必须显示
  正确的水平、垂直或对角缩放光标。
- 热区只能向窗口客户区内扩展，必须检查贴边按钮、滚动条和最外层岛屿是否被夺取输入；
  最大化、全屏、固定尺寸窗口关闭该热区。

如果短期只能修改 Linux 常量，可以作为有测试的本地框架补丁落地，但计划终点仍是
上游可配置或统一的平台策略；补丁必须记录在 HuxerUI 本地补丁清单，不能静默漂移。

### 15.3 执行顺序与 agent 边界

#### P1-B0.6A：应用标题栏拖动区

- 在 P1-B0.5 完成后执行；两批都会修改 `app.cpp`，禁止并行写同一文件。
- 只调整标题栏子项与命中语义，不顺手重做标签视觉、按钮尺寸或顶级状态模型。
- 增加可自动化的布局/命中测试时，至少覆盖“弹性空白可拖动、真实控件仍可点击”。

#### P1-B0.6B：HuxerUI 缩放热区

- 可由独立 framework agent 处理，仅修改 HuxerUI 窗口 API、平台适配、框架测试和对应
  文档；不得顺手修改 apitab 页面。
- 先验证 Linux 6 DIP 根因，再决定公开配置还是平台默认策略；Windows/macOS 不凭猜测
  改行为。
- 若公开头发生变化，B0.6B 与 P1-B1.0a 合并交付 SDK；否则保持为有来源记录的框架补丁。

#### P1-B0.6C：集成与实机验收

- QA agent 最后执行，不新增产品功能；记录显示缩放比例、窗口状态和平台。
- B0.6 未完成不阻塞菜单模型设计，但涉及 `app.cpp` 的执行批次必须串行，避免标题栏
  结构冲突。

### 15.4 验收矩阵

- 鼠标从 Logo 无动作区、标签条空白、标签与设置按钮之间的弹性空白均可拖动窗口；
  起拖后指针离开标题栏仍能连续拖动。
- 点击和拖动主页/项目/设置标签、标签关闭、闪电、设置及系统窗口按钮时，不误拖窗口；
  hover、按下、焦点环和键盘激活保持正常。
- 双击标题栏空白区按平台习惯最大化/还原；窄窗、Compact、标签溢出/横向滚动时仍有
  合理的拖动空白，不以覆盖标签换取拖动面积。
- 四边分别可稳定水平/垂直缩放，四角可稳定对角缩放；在 100%、125%、150%、200%
  缩放下命中宽度体感一致，光标方向正确。
- 最大化、全屏和不可调整大小窗口不出现缩放光标；窗口边缘附近的按钮、滚动条与岛屿
  内容在热区之外仍可正常操作。
- Linux 为本批必测平台；Windows/macOS 至少完成原生行为回归。最终同时通过源码模式、
  已安装 SDK 模式和仓库离线 SDK 模式构建，`git diff --check` 无格式错误。

### 15.5 P1-B0.6 代码收口结果（2026-09-02）

- **B0.6A 已回修**：`TopTabStrip` 不再占满标题栏。标签条按实际标签内容计算自然宽度，
  窄窗时受父约束并保留横向滚动；标题栏只保留一个 `Grow(1)` 空白 sibling，并明确
  标记为 `WindowDragRegion`。闪电、设置和系统窗口按钮恢复固定可见，标签/关闭/动作
  控件继续命中 Client，Logo 与标签右侧整块真实空白命中 Drag。
- **B0.6B 已落地**：Linux 四边热区 10 DIP、四角 14 DIP；按下缩放和 hover 光标共用
  `ResolveLinuxResizeEdge`。适配器保存应用请求的 Hand/Text 等光标，缩放边缘优先，
  离开边缘或最大化后恢复应用光标；无效坐标、越界、零尺寸和最大化均不返回缩放边。
- **自动验证通过**：apitab 源码构建、CLI `huxerui build linux`（确认使用
  `third_party/huxerui @ 937efb1`）、`test_smoke`、选择矩阵 13/13、Linux resize 定向
  测试 16/16、既有标题栏指标测试 9/9、`git diff --check`。
- **实机外观已复核**：新 `.huxerui/build/linux/debug/apitab` 中 Logo、标签、闪电、
  设置与系统窗口按钮均在同一标题栏正常显示，标签与动作组之间保留大块空白。
- **仍待人工输入验收**：Linux 四边/四角实际拖动手感、边界内外 1px、最大化禁用、
  离开边缘恢复 Hand/Text，以及 Logo/标签后空白的真实拖窗；Windows/macOS 原生行为
  回归和 SDK 三通道发布验证继续归 P1-B1.0a。未完成这些人工项前不把 B0.6C 标为完成。

## 十六、P1-C：大文件按功能域拆分

### 16.1 背景与范围

当前业务源码中两个文件已超过 1000 行，职责边界开始影响 UI 修复、源码优先同步和
agent 并行开发：`src/ui/request_page.cpp`（约 2900 行）同时承载请求编辑、请求列表、
环境控件、响应区、Body 编辑器、导入弹窗和动作按钮；`src/ui/app.cpp`（约 1560 行）
同时承载应用壳、标题栏、顶级标签、全局状态条和多个对话框。本任务只做保持行为的
结构重构，不顺带改变视觉或交互。

### 16.2 执行顺序

1. **P1-C1（最高优先）**：拆分 `request_page.cpp`。建议边界为：
   `request_editor.cpp`、`request_list.cpp`、`request_tab_strip.cpp`、
   `request_body_editor.cpp`、`request_response.cpp`、`environment_widgets.cpp`、
   `request_actions.cpp`、`api_import_dialog.cpp`。首批先抽出 `RequestListIsland`、
   `RequestEditor` 和环境相关控件，避免一次性大范围移动。
2. **P1-C2**：拆分 `app.cpp`。建议边界为 `app_shell.cpp`、`title_bar.cpp`、
   `global_status_bar.cpp`、`app_dialogs.cpp`；`AppRoot` 保持薄编排层。
3. **P1-C3**：整理公共声明。组件跨文件共享时，新增职责清晰的局部头文件；不得把
   页面私有状态继续堆入 `src/ui/ui.h`。保留 `.cpp` 作为 hcg 输入，禁止直接编辑生成的
   `build/hcg` 文件。

### 16.3 约束与验收

- 每次只迁移一个功能域，迁移前后 UI 行为、状态槽位、事件顺序和菜单语义不变。
- 新文件原则上控制在 800–1000 行以内；小组件不为凑文件数量拆分。
- 每个阶段必须通过 `cmake --build build --target apitab test_smoke`、相关定向测试、
  `git diff --check`，并确认 source-first 的 HuxerUI 构建路径未改变。
- 记录新增/移动的 composable、共享类型和 include 依赖，避免循环依赖；完成后更新本节
  状态和文件行数统计。
- P1-C 不与标题栏、窗口缩放、ComboBox 或菜单交互修复混合提交；若发现行为回归，先
  回退结构迁移再单独修复。

### 16.4 P1-C1 阶段 1 执行记录（2026-09-02）：请求集合树 → request_list.cpp

从 request_page.cpp 拆出功能域「请求集合树」到新文件 `src/ui/request_list.cpp`
（764 行）；request_page.cpp 由 2897 → 约 2178 行。本阶段为**纯逐字搬移**，
组合逻辑、状态槽位、事件顺序与菜单语义未改；source-first 构建路径未受影响。

- **迁移到 request_list.cpp**（全部原样搬移；`RequestListIsland` 为跨 TU 调用，
  由匿名命名空间提升为 `apitab::ui` 外部链接）：
  - composable：`RequestListIsland`（左岛集合树）、`RowMenuButton`（行尾 ⋮，被
    island 独占，仍为文件私有）；
  - 纯函数/结构：`DraftFromSaved`（db::SavedRequest → RequestDraft，含用例/Mock
    子对象载入）、`CaseFromDb`、`MockFromDb`、`RequestDragPayload`、
    `GroupDragPayload`。
- **ApiImportDialogContent**（导入接口弹窗，被左岛“+”菜单调用）仍在
  request_page.cpp，但从匿名命名空间提升为外部链接、经 ui.h 声明供集合树 TU 调用；
  其依赖 `ImportedBodyKindIndex` / `InferKvType` 仍为该 TU 私有实现。
- **桥接并列说明**：`FromKeyValue`（api::KeyValue → KvRow，签名含模块类型、普通头
  无法声明，见 CLAUDE.md 模块约束）在 request_list.cpp 内按需并列一份，注明与
  request_page.cpp（环境变量表单）逻辑相同；P1-C3 整理公共声明时归并唯一实现。
- **ui.h** 新增 `RequestListIsland` / `ApiImportDialogContent` 声明（含职责注释），
  未向 ui.h 堆入任何页面私有状态。
- 验收：`cmake --build build --target apitab` 通过（仅既有第三方头
  `-Wsubobject-linkage` 警告）；`ctest` 2/2（smoke + 选择矩阵）；CLI 冒烟正常；
  `git diff --check` 干净。
- 待续：RequestEditor（含 SpecFromDraft/CookiesFromHeaders/KvTable 编辑器侧）、
  RequestTabStrip、ResponseArea、BodyTextEditor/文档页、环境控件等后续功能域继续
  从 request_page.cpp 拆出（P1-C1 剩余），之后 P1-C2（app.cpp）、P1-C3（公共声明）。

### 16.5 P1-C2 执行记录（2026-09-02）：应用壳拆分 → title_bar / global_status_bar / app_dialogs

`src/ui/app.cpp` 同时承载标题栏、顶级标签、全局状态条、对话框与 AppRoot 薄编排，
按 §16.2 建议边界拆为职责文件；AppRoot 保持薄编排（仅状态持有与路由），行为零改动。

- **新文件（3 个，职责不混合、每文件 ≤ ~1000 行）**：
  - `src/ui/title_bar.cpp`（509 行）：标题栏结构——`LogoBadge`（apitab_mark 矢量徽标）、
    `TopTab`（单个标签，激活/悬停/关闭，键盘 Focusable/Semantics）、`TopTabStrip`
    （主页钉最左、项目标签横向滚动、设置单例固定队尾、分隔竖线、拖拽换位与让位滑动、
    覆盖层克隆）、`ProjectTabDragPayload`、`TopTabDisplayKey`；§15 标题栏命中区规则
    （Logo/标签空白=拖动区、标签本体/✕/闪电/齿轮=交互区、Grow(1) WindowDragRegion 弹性空白）
    原样保留，拖拽 payload/行为不变。
  - `src/ui/global_status_bar.cpp`（358 行）：底部全局状态条——`TruncateSummary`、
    `CookieRow`、`CookieRowsFromStore`、`StatusActionText`、`RequestProxyDialogContent`、
    `GlobalCookieDialogContent`、`GlobalStatusBar`（右缘代理/Cookie 热区，弹窗层捕获调用处
    环境；P1-B1 前保持现状，不拆为项目/状态两栏）。
  - `src/ui/app_dialogs.cpp`（104 行）：应用级对话框宿主——`CloseGuard`（关闭询问：
    直接关闭/最小化到托盘/取消，托盘可用性动态查询，Hide/Quit 推迟出事件路径）。

- **保留为壳（原 app.cpp 薄编排层，履行 §16.2 app_shell 角色）**：
  - `src/ui/app.cpp` 由 1561 → 670 行，保留：`pages::PageIndex/PageFor`（项目工作区路由）、
    `MinimalDarkThemeSpec/MinimalLightThemeSpec/MinimalThemed`（极简黑白主题，8px 按钮圆角
    覆盖）、`SideShell`（左侧图标侧栏）、`AppRoot`（全部受控 State：tabs/settingsOpen/
    activeTopTab/lastProjectTab/navPage/themeMode/closeBehavior/settingsCategory + 统一
    入口 activateTopTab/closeTopTab/commitTopTab/syncDomainProject，推迟任务约定 6）。
  - `src/ui/ui.h` 新增 P1-C2 块：跨 TU 共享常量 `kTitleBarContentHeight/kProjectTabWidth/
    kSettingsTabDisplayKey`（原匿名常量提升为头常量，TopTab/TopTabStrip 与 AppRoot 共用）、
    `TopTabActions` 结构、`LogoBadge/TopTabStrip/GlobalStatusBar/CloseGuard` 声明（签名仅用
    State/draft.h/std/huxerui 类型，可安全进头）；不含模块类型声明（CLAUDE.md 头文件墙）。

- **app.cpp 前后行数**：1561 → 1084（标题栏抽离后）→ 755（状态条抽离后）→ 670（对话框抽离后）；
  新文件合计 509+358+104=971 行，整体代码量不变（纯搬移）。

- **桥接并列清单与理由**：本次拆分**无模块类型桥接并列**。标题栏/对话框签名仅用
  huxerui::State 与 TopTabId，状态条的 `CookieRowsFromStore`（db::GlobalCookie）与
  `TruncateSummary` 等 helpers 按 CLAUDE.md 约束保留在各自 TU 内私有，不进 ui.h；
  虽与 request_page 的 `FromKeyValue` 同属模块类型签名场景，但本批未复制该类逻辑，
  无需并列与 P1-C3 归并。

- **验证（每个功能域提交前必做）**：
  - 配置日志含 `HuxerUI 源码编译（third_party/huxerui @ f85a94f）` 与 `宿主工具用本地源码构建`；
  - `cmake --build build --target apitab -j8` 通过（仅既有第三方头 `-Wsubobject-linkage` 警告）；
  - `cmake --build build --target test_smoke && ctest --test-dir build -R smoke` 通过（1/1）；
  - `git diff --check` 干净；`./build/apitab --cli help` 冒烟正常；
  - 行为零改动：顶级标签行为矩阵 §13.6、标题栏拖动 §15、键盘可达性（Focusable/Semantics）
    保持不变，State 槽位顺序与推迟任务语义未变。

- **留给 P1-C3**：
  - 若后续出现跨 TU 复用的模块类型 helpers，再评估微小桥接并列与归并（当前无）；
  - `app.cpp` 仍可进一步抽 `SideShell` 为独立组件或将主题派生抽至 `theme.cpp`，但非必须
    （当前 670 行已满足 ≤1000 约束，AppRoot 已是薄编排）；
  - ui.h 的 P1-C2 块与 P1-C1 块同处一文件，后续按职责拆局部头时再迁移，不在本批堆局部头；
  - 继续 P1-C1 剩余（RequestEditor/TabStrip/Response/Body/Environment 等）与 P1-C3 公共声明整理。
