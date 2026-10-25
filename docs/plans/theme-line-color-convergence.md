# 计划：GitHub 中性线条体系统一与页面区域重构

状态：**实现及 Linux 双主题初次截图抽查、构建已完成（2026-10-24）；2026-10-25 完成全局线宽收尾并构建通过，收尾后截图、跨平台和缩放矩阵待实机补验**。

## 目标

apitab 采用 GitHub 风格的中性灰阶，以线条组织主内容；品牌青保留给焦点、主要操作和
明确的交互状态。本计划把全项目边界线、分隔线和控件描边收束到同一主题色系，再按统一
规则逐区整理页面。它延续 `docs/plans/island-structure-theme.md` 的线条风几何约定，
只收束线条颜色与其使用方式，不恢复岛屿卡片层级。

## 实施前基线与问题

- `src/ui/app.cpp` 中深色画布为 `#0d1117`、outline 为 `#3d444d`、次级前景为
  `#9198a1`；浅色画布为 `#ffffff`、outline 为 `#d0d7de`、次级前景为 `#59636e`。
  主体保持中性，青色 `primary` 用于品牌和焦点。
- `src/ui/common.cpp::ResolveIslandTheme` 将页面 hairline 设为 `outline` 的 75%
  透明度；`IslandTheme::outline_soft` 则是全强度 outline。名称与强弱含义容易混淆。
- 仍有页面自行从 `theme.colors.outline` 计算线条：请求页 `ResizeHandle` 使用 50% 透明度；
  标签条、标题栏、菜单分隔、主页项目卡片等直接取全强度 outline；其它页面则取
  `outline_hair`。规则分散，线条在不同页面显得深浅不一。
- 请求页环境选择器目前用 `on_surface_variant` 的 55% 透明度做外框，和中性 outline
  系列不同。该局部增强应迁入统一主题线条令牌，避免继续增加页面私有色值。
- `src/ui/app.cpp::MinimalThemeStyles` 已向 TextField、Select、Chip、SegmentedButton、
  Divider、Menu 和 Dialog 注入 outline，但自绘组件并未全部使用同一语义层。
- 相邻面板只保留一条分隔线、面板不再各自描边加间隙的结构约定继续有效。全局设置页
  已改为单外框加一条内部竖线，本计划保留该结构。

## 统一视觉规则

结构与普通控件描边统一为 `kLineWidth = 0.5F` HuxerUI 逻辑尺寸；最终物理像素宽度随
窗口缩放映射。焦点环与活动指示线使用 `kEmphasisLineWidth = 2.0F`，只表达交互状态。
头像裁剪框等媒体画布内容按自身对比度需要绘制，不作为面板结构线。所有静态线条从当前
深/浅主题的中性 outline 派生，不在页面或组件里混用次级文字色、品牌
色或独立灰值。只保留少量按职责区分的语义档位：

| 角色 | 用途 | 规则 |
|---|---|---|
| `line_subtle` | 内容区之间的装饰分隔、表格行分隔、普通输入框边界 | `outline → on_surface_variant` 插值 30%；不承担交互状态表达 |
| `line_default` | 窗口骨架、输入/选择器等控件外框、悬停输入框边界 | 同一插值 60%；关键边界深浅主题均达到 3:1 |
| `line_focus` | 键盘焦点、活动输入、明确的当前选择 | 主题 `primary` 青色；只表达状态，不作为常态结构线 |
| `line_danger` | 删除等危险状态的显式反馈 | 主题 `error`；不能用于 HTTP 方法色或一般分隔 |
| `line_disabled` | 禁用控件边界 | 保持 `line_subtle` 色相并使用 0.45 alpha |

`IslandTheme` 统一提供上述语义令牌，页面和控件不再读取含义不清的
`outline_soft` / `outline_hair`，也不从次级文字色自行派生或叠加局部 alpha。
按 8-bit sRGB 取整并以 WCAG 相对亮度公式计算，当前令牌约为：深色
`line_subtle #565d66`（2.84:1）、`line_default #6f767f`（4.12:1）；浅色
`line_subtle #acb4bc`（2.10:1）、`line_default #89919b`（3.19:1）。纯装饰线
允许低于 3:1，但不能在 100% 缩放下消失；主要边界达到 3:1。颜色由
`ResolveIslandTheme` 集中从 `ThemeSpec` 计算，不逐页修饰。

交互态约定：普通输入框默认边界用 `line_subtle`，hover 用 `line_default`，focus 使用
`line_focus` / 焦点环；disabled 使用 `line_disabled`。窗口骨架和控件外框用
`line_default`，静态内容分隔用 `line_subtle`；普通页面分区不画外框，浮层/危险面保留
对应语义线。页面不自行从 `on_surface_variant` 混合出边框颜色。

## 区域清单与改造顺序

### P0：建立主题线条令牌与映射表

涉及：

- `src/ui/app.cpp`：`MinimalDarkThemeSpec`、`MinimalLightThemeSpec`、
  `MinimalThemeStyles` 中框架组件的边界色注入。
- `src/ui/ui.h`：`IslandTheme` 及线条颜色字段。
- `src/ui/common.cpp`：`ResolveIslandTheme`、`IslandSurface`、弹层与组合控件的公用线条。

工作项：

1. 定义唯一的线条色源和少量语义档位；确认深浅主题颜色、透明度和与底色的对比目标。
2. 明确 `ThemeSpec::colors.outline`、页面边界、交互控件边界、hairline、focus ring 的
   映射；框架自带组件和自绘组件共用此映射。
3. 删除或重命名 `outline_soft` / `outline_hair` 的歧义注释与字段；禁止新增页面私有的
   `outline.alpha *= ...` 或从次级文字色派生线条。
4. 先做深浅主题并排色板，确认静态轮廓层级后再迁页面，避免逐页试色。

验收：主题定义成为静态线条颜色的唯一来源；任一颜色档都能说明自己的视觉职责。

### P1：应用骨架与顶级导航

涉及：`src/ui/app.cpp`、`src/ui/title_bar.cpp`、`src/ui/global_status_bar.cpp`。

区域：标题栏下缘、侧栏右缘、状态栏上缘、顶级标签分隔、主页/设置/项目导航边界。

工作项：

1. 把窗口骨架全强度线统一映射到 `line_default`，不在主窗口和子窗口各维护一套色值。
2. 顶级标签之间的短分隔、状态栏局部边界按装饰线或交互边界归类，不再直接散取 outline。
3. 保留青色焦点/选中指示，不把结构线整体染成品牌色。

验收：主窗口、子窗口、深浅主题下同类骨架线同色同强度；导航状态色与结构线容易区分。

### P1：请求工作区与输入工具栏

涉及：`src/ui/request_page.cpp`、`src/ui/request_tab_strip.cpp`、
`src/ui/request_editor.cpp`、`src/ui/request_editor_kv.cpp`、
`src/ui/request_response.cpp`、`src/ui/common.cpp`。

区域：左侧请求树/编辑区分隔、可拖动 ResizeHandle、标签条、环境选择器、MethodUrlBar、
参数/头部表格、编辑分区切换、响应区及 HTML Preview 宿主边界。

工作项：

1. `ResizeHandle` 移除独立 50% 透明度计算，使用 `line_subtle`；hover/drag 继续使用
   `line_focus` 或已定义的活动色档。
2. 环境选择器将当前 `on_surface_variant × 55%` 外框迁至 `line_default`；和 MethodUrlBar、
   TextField、ComboBox 使用同一交互控件边界规则。
3. 检查标签条、环境选择器内部竖线、编辑分区线和响应区边缘，区分结构线与状态线，删除
   多余叠线；环境选择器所在组保持单一外框。
4. 对 SweetEditor 和 WebView 只补充必需的宿主边界；编辑器内部语法色、选区色不纳入
   结构线令牌，避免主题线条规则污染代码高亮。

验收：请求树、URL 工具栏、标签/环境控件、编辑区和响应区的边界在同一主题内一致；
Preview/WebView 内容不因父子描边叠加出现双线或裁切。

### P2：主页、历史、压测和项目设置

涉及：`src/ui/home_page.cpp`、`src/ui/history_page.cpp`、`src/ui/loadtest_page.cpp`、
`src/ui/project_settings_page.cpp`、`src/ui/testcase_page.cpp`、`src/ui/settings_page.cpp`。

区域：组织/项目分栏、项目卡片 hover/focus 边缘、历史分类栏/表格/分页边界、压测分区、
项目设置表单、测试用例行卡、全局设置分类栏与内容区。

工作项：

1. 扫描直接使用 outline、hairline、局部 alpha 或透明描边的调用，逐个归入
   `line_subtle`、`line_default`、`line_focus`。
2. 交互卡片 hover 与 keyboard focus 不共用一个无状态颜色；focus 清晰采用 `line_focus`，
   hover 维持中性线或状态底。
3. 表格行分隔、表格外边界、分页栏边界分别定级；不要每行同时加 Border 和分隔线。
4. 保留全局设置页单外框和唯一分类竖线，继续检查其余页面的相邻边是否形成双线。

验收：每个页面只有必要结构线；线条强弱由语义决定，不由页面历史实现决定。

### P2：框架控件、菜单与对话框

涉及：`src/ui/app.cpp::MinimalThemeStyles`、`src/ui/common.cpp`、各页面的 SearchablePicker
和表格/表单组件。

区域：TextField、Select、SegmentedButton、Chip、Divider、Menu separator、Dialog action
separator、自绘 PopupMenu 和弹层边界。

工作项：

1. 框架默认控件的 `border`、`hovered_border`、`focused_border`、`disabled_border` 使用
   同一套 semantic mapping。
2. 菜单/对话框外框用 `line_default`，菜单内部分组和 action separator 用 `line_subtle`。
3. 检查本地覆写透明 Border 的组件；只在需要防几何跳动时保留透明描边，并说明原因。
4. 确认焦点环与焦点边框不会重复造成双层青色轮廓。

验收：相同类型控件跨页面、深浅主题和 Compact/标准布局使用同一线条规则。

### P3：全量清理与视觉回归

1. 用仓库搜索核对 `theme.colors.outline`、`outline_hair`、`outline_soft`、局部 alpha、
   `Border`、`Divider` 和使用 `kLineWidth` 的背景分隔线；每处留有明确的语义归属。
2. 删除过期的“outline 半透明档”等注释；更新 `docs/plans/island-structure-theme.md`
   的线条风交叉引用，不重写其历史实施记录。
3. 在深色/浅色、100%/125%/150% 缩放、主/子窗口、Compact/标准视口下截屏对比；重点
   看文字与线条相邻处、线条交汇点、面板边缘和鼠标焦点状态。当前已在 Linux 运行态抽查
   深色主页与浅色请求入口，其他矩阵仍待实机补验。
4. 运行 `cmake --build build --target apitab --parallel $(nproc)`；按任务要求执行项目验证，
   汇总未解决的 Linux/Windows/macOS 差异。

## 非目标

- 不改变 GitHub 中性骨架、品牌青、HTTP 方法色、危险色和状态底色的语义职责。
- 不借线条收束顺手重做页面信息架构、控件尺寸、间距、动画或整个主题配色。
- 不恢复面板自带四边 Border + 间隙的旧布局；几何仍遵守“相邻区域只画一条线”。
- 不把代码编辑器语法高亮、HTML 内容、头像裁剪框等内容内绘制色并入结构线体系。

## 完成标准

- 整个 UI 的静态线条只从主题定义的中性线条系列派生，页面没有私有 RGB 或临时 alpha。
- 结构边界、控件轮廓、装饰分隔、焦点和危险状态有清楚层级；同类元素跨页面一致。
- 深浅主题都可辨，关键交互边界清楚；纯装饰线不抢正文，但不会在常用缩放下消失。
- 没有相邻双线、面板旧 Border 残留、边框被裁切或焦点双重描边。
- 构建成功；计划实施的提交只包含当前批次文件，并推送当前分支上游。

## 实施记录（2026-10-24）

- `IslandTheme` 统一为 `line_subtle`、`line_default`、`line_focus`、`line_danger`、
  `line_disabled`；`ResolveIslandTheme` 集中由 GitHub 中性主题色计算，页面不再各自
  取 outline、混次级文字色或叠局部 alpha。
- 应用主/子窗口骨架、标题标签、请求页分隔与组合栏、主页、历史、压测、项目/全局设置、
  测试用例、框架输入/选择器/菜单/对话框统一迁移到语义令牌。输入框常态与 hover 分档；
  环境/URL 组合栏在获得焦点时由外层单边界切换为品牌焦点色，避免内部双描边。
- `rg` 审计后，直接读取 `ThemeSpec::colors.outline` 的位置只保留主题定义与统一令牌解析，
  以及 WebSocket/TCP 的“已关闭”状态色；头像裁剪框是内容叠层，不属于结构线。KV 表格单元格
  保持透明常态描边，代码内说明这是为了避免密集网格，hover/focus 仍采用全局主题线色。
- Linux 当前窗口截图检查：深色主页与浅色请求入口均为 2360×1520；可见主骨架和页面分区线
  清晰，抽查区域未见叠加双线。Linux 的其他页面/焦点交互、125%/150% 缩放、子窗口以及
  Windows/macOS 尚未截图验证。
- 全局结构线与普通控件边框统一为 `kLineWidth`（0.5 HuxerUI 逻辑尺寸）；输入框聚焦、
  校验、复选框/单选框/开关不再继承框架的 2.0 宽边框。焦点环和活动下划线共用
  `kEmphasisLineWidth`（2.0），保留状态可见度；标签布局步进按实际 `kLineWidth` 计算。
- 页面分区取消常态外框，边界只由布局层的单条分隔线表达；测试用例记录以横向分隔线
  分组，避免“卡片描边 + 间隙”形成重复轮廓。
- `cmake --build build --target apitab --parallel $(nproc)`：通过（Linux GCC 16.2.1，源码版
  HuxerUI，WebView 可用）；`git diff --check` 通过。构建输出包含 HuxerUI coroutine
  lambda 的 `-Wsubobject-linkage` 警告，无编译或链接错误。
