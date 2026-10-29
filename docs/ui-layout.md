# 当前 UI 布局与尺寸约束

本文对照当前应用源码记录布局，不把源码中的 `Island` 命名理解为仍在使用岛屿风格。
尺寸均为 HuxerUI 逻辑尺寸（dp），不是物理像素；`Frame` 的目标尺寸与最小尺寸仍服从父约束。

## 外壳与表面

主窗口入口在 `src/app.cpp`，布局在 `src/ui/app.cpp`，拆出的项目窗口使用
`ChildWindowRoot`。主/子窗口初始尺寸都是 1180×760，最小尺寸都是 800×600。
应用统一使用 900/1200dp 视口断点（`kAppViewportBreakpoints`）：800dp 桌面窄窗进入
Compact，900dp 起常规分栏，1200dp 起 Expanded。

```text
MinimalThemed
└─ 根 Column（CrossAlign Stretch）
   ├─ WindowTitleBar：Logo / 主页 / 项目与设置标签 / 操作区 / 系统窗口按钮
   └─ 主 Row（Grow + CrossAlign Stretch）
      ├─ SideShell：仅项目工作区显示
      └─ IndexedPages：主页 / 各项目工作区 / 已打开的全局设置
         └─ 每个顶级页面各自包一层 PageCard：外边距、外轮廓与裁剪
            ├─ 页面内容（Grow）
            └─ 0.5dp 内部分隔线 + 28dp 状态栏：仅项目卡片显示
```

子窗口直接显示单个项目的 `PageCard`，不再套顶级 `IndexedPages`。项目工作区内部的
请求、压测、历史、项目设置由另一层 `IndexedPages` 切换。切换只改变选中页面；关闭
标签、改变 Key 或条件分支移除节点才会卸载相应子树，不能据此声称所有请求编辑器也始终保活。

三层表面应分别描述：

| 层级 | 当前契约 |
|---|---|
| 窗口画布 | 深色为 `#000000`；标题栏、项目侧栏直接使用画布 |
| PageCard | 外边距 8dp，圆角取 shapes.large（当前 10dp），0.3dp 前景描边；内容矩形裁剪、前景遮角，无额外内容内缩；底色深色 `#303030`、浅色 `#f6f8fa` |
| 页面工作面 | `islands.base`：深色 `#121212`、浅色 `#ffffff`；普通分区无边框、直角，相邻区域只画一条 0.5dp 线 |

页面工作面会覆盖其下方的卡片底色；卡片底色不等于所有内部分区的底色。普通
`IslandSurface` 的 Base/Raised/Active 都使用同一工作面，列表行 hover/选中才单独使用
`islands.raised/active`。浮层与危险面保留自己的底色和边界。

外框由 `page_card.h::PageCardSurface` 统一实现：Stack 的第一个 child 是矩形裁剪的内容层，
中间 Canvas 按窗口画布色填充四个圆角外侧，最后是没有背景及交互的纯描边层。
内容和描边共享尺寸，前景绘制保证外框不会被工作面覆盖；描边深色用白色、浅色用 line_default。
不为描边额外加 Padding 或内容内缩。
不能给 Stack 加圆角 ClipChildren：原生 WebView 不支持祖先路径裁剪，Linux 收到 HTML
后打开 Preview 会直接抛异常。圆角遮角层不向子树传播路径裁剪；遮角和描边层不绑定事件、焦点或语义。

项目状态栏作为卡片 footer 与工作区一起裁剪，上方使用官方 Divider，局部 DividerStyle
继承当前主题并将线色设为 `line_default`、粗细设为 `kLineWidth`。
它与工作面同底色、高 28dp，左右留白 12dp、上下 2dp；卡片保留底部 8dp 外边距，
状态栏不再跨过导航侧栏或贴着窗口底边。主窗口与项目子窗口使用同一布局，主页与全局设置不显示状态栏。

标题栏期望高度为 24dp，不是所有子项及最终标题栏都强制为 24dp：主窗口头像为 28dp，
系统窗口按钮也可能提高高度下限。根 Column 不设置分区间距，页面外边距由 PageCard 提供。

## 页面分区与滚动

| 页面 | 常规分区 | 滚动所有者 |
|---|---|---|
| 主页 | 常规左组织栏 240dp；Compact 顶部满宽组织栏 160dp；项目区 Grow，中间单线 | 组织 VirtualList；项目 VirtualGrid，自适应列数，卡片 200×96dp |
| 请求 | 左请求树，右编辑/响应区，分区由拖条分隔 | 树、编辑内容和响应各自滚动；固定工具栏不随内容滚动 |
| 压测 | 常规左用例树 250dp；Compact 顶部满宽用例栏 160dp；配置区 Grow | 左树滚动；右侧整体 ScrollView，其中输出另有 260dp 高的 VirtualList |
| 历史 | 左侧 320dp：标题、请求/压测分段、按请求分组记录；右侧多标签只读详情；Compact 上下分区 | 左侧分组列表与右侧详情各自滚动；标签条横向滚动；请求历史分页栏固定在左侧底部 |
| 全局设置 | 左分类栏，右设置内容 | 左栏固定，右侧 ScrollView；分类栏宽按 192dp 基准扣除外侧缩进 |
| 项目设置 | 元信息、公共请求头、保存按钮单列排列 | 公共请求头表 VirtualList，不再外套整页 ScrollView |

全局设置的根工作面显式使用 `islands.base`，覆盖分类栏、表单、关于页和四周留白，
避免透明区域露出 PageCard 的底色。页面局部继承现有 TextField/Select/SegmentedButton
样式：输入框、下拉触发器和分类选择器统一使用 `surface_container`；选中项与下拉弹层
使用 `surface_container_highest`，正文及次级文字继续取 `on_surface/on_surface_variant`。
分类 hover 使用 `surface_container`，头像编辑徽标也随主题取色，深浅主题不再混用固定灰白色。
局部样式由 `settings_theme.h::SettingsPageTheme` 封装。Theme 是纯环境节点，不能附加
Grow、Frame、背景或过渡锚点；这些修饰必须放在内部页面容器上，否则挂载时会抛出
`HuxerUI Environment node cannot declare view behavior`。回归测试通过点击打开设置主题，
验证深浅模式的挂载、输入、关闭及窄窗重开。

HTTP 调试页右侧按以下层级布局：

```text
右侧 Column
├─ 编辑区（Grow；期望最小 480×300dp）
│  ├─ 请求标签条：横向滚动标签 + 环境组合控件
│  └─ RequestEditor
│     ├─ 调试 / 文档 / 测试用例 / Mock + 请求名称
│     ├─ 方法 / URL / 发送 / 保存 / 更多
│     ├─ Auth / Params / Headers / Cookies / Body / 设置
│     └─ 当前分区：固定头 + 独立滚动内容
├─ 横向 ResizeHandle
└─ 响应区：Body / Headers / Cookies / 可选 Preview
```

请求树宽度 State 默认 260dp，常规拖动范围 180～420dp，Compact 上限为 220dp；
响应高度 State 默认 260dp，范围 140～640dp。`SplitPane` 在每次测量时先为编辑区保留
480dp 宽度和 300dp 高度，再用剩余空间限制请求树/响应区。父空间连下限都无法满足时，
可调分区继续缩小到零，防止负尺寸和重叠；正常桌面最小窗口仍可同时保留两个分区。
拖条独占 6dp 命中热区，中央只画
0.5dp 线，悬停/拖动只改变线色，不叠在面板边缘，也不附加第二条分隔线。

文档、测试用例和 Mock 隐藏响应区；WS/TCP 使用整个右侧会话区，顶部操作固定、事件流
独立滚动；gRPC 是占位。新建请求的 Flow 按实际可用宽度换行，没有固定两列契约。

KV 表与代码编辑器拥有自己的滚动，调用方不再追加同轴 ScrollView。响应 Body 完成态
使用只读编辑器，传输中使用流式 ScrollView；Headers/Cookies 使用 VirtualList。

## 约束修正与验证

- **桌面窄窗策略。** 断点从默认 600/840dp 调整为 900/1200dp，让 800dp 最小桌面
  窗口实际使用 Compact。主页、压测页把导航列表移到顶部；历史和设置使用已有的顶部
  分类切换。请求页保留左右结构并收窄请求树，避免顶部树继续挤占编辑/响应区高度。
- **分区预算。** 旧请求页的默认宽度需求为 826dp，默认高度需求至少为 628.5dp，超过
  800×600 窗口。现在在实际父约束内分配尺寸，保留编辑区 480×300dp；树宽和响应高度
  不再按固定范围无限挤压对侧。测量不改写用户偏好尺寸，重新放大后会恢复可容纳的请求值。
- **拖拽连续性。** 拖动起点读取本次实际分配尺寸，增量根据实时可用范围限幅，避免
  窗口缩小后从不可见的旧请求尺寸开始拖动而产生跳变。几何结果只保存普通值，不在测量
  阶段读写 State，也不保存节点引用。
- **主页尺寸声明。** 组织栏在组合时选择常规 240dp 宽或 Compact 160dp 高的 Frame，
  不再叠加 240dp 最小高度与 220dp 最大高度。Compact 宽度由外层 Stretch 提供。
- **压测页滚动。** 顶部用例栏独立滚动，配置区取得剩余高度并整体滚动。输出仍有意使用
  260dp 高的虚拟列表；实际平台的滚轮边缘交接需保留人工体验验证。

`SplitPane` 是应用侧的小型测量布局：Row/Column 无法在固定分区测量前按对侧保留空间
动态限制它，因此仅将这部分分配算法放入自定义 Layout；输入、State、表面及滚动继续由
普通组件负责。它支持横向/纵向、有界/无界约束，不使用测量结果驱动持续重组。

回归测试 `tests/test_ui_layout.cpp` 使用 HuxerUI 公开的无窗口测试库，覆盖分区缩小与
恢复、请求尺寸两端限幅、父空间不足/零高度、无界滚动测量，以及 800/900dp 断点；
也验证遮角路径、深浅主题下的卡片与 footer 尺寸，以及前景层下方的点击、文本输入和滚轮路由。
源码通道启用应用测试时构建 `test_ui_layout`，CTest 名为 `ui_layout`；离线 SDK
通道不要求随包提供该可选测试库。

Linux 源码通道且 WebView 可用时，`tests/test_preview_native.cpp` 运行真正的 GTK/WebKit
窗口，验证深浅主题下卡片内 HTML 加载、切走卸载及重新挂载。CTest 名为
`preview_native_dark` / `preview_native_light`，无 DISPLAY/WAYLAND_DISPLAY 时跳过。
它覆盖原生控件合成契约，不能由无窗口布局测试替代。

本次源码通道完成应用编译链接、`ui_layout` 及深浅主题的原生 Preview 回归。
强制使用已安装 SDK 的整体构建
在现有 Lib-WebView 依赖处失败：缺少 `huxerui/linux/platform_registry.h`，尚未进入
应用链接；新增 SplitPane 使用的公开布局 API 另行做 SDK 头文件编译检查。该依赖差异
独立于本次布局修改，不能把头文件检查等同于整个 SDK 通道构建成功。

维护入口：外壳 `src/ui/app.cpp`；断点 `src/ui/app.h`；分配算法 `split_pane.h`；尺寸
及分派 `request_page.cpp`；编辑器 `request_editor.cpp`；请求标签 `request_tab_strip.cpp`；
表面令牌 `common.cpp` / `ui.h`。改变尺寸、断点或滚动所有权时同步更新本说明。

## 分隔线、色板与控件约束

- 页面根不得再画周边线或额外外框；普通分区不使用四边 Border + 相邻间隙，
  避免叠成双线。相邻面板之间只画一条 `kLineWidth = 0.5F` 逻辑尺寸线。
- 静态分隔用空 Row/Column + Frame 的 width/height = kLineWidth，
  Background(line_subtle)，靠父容器 CrossAlign(Stretch) 拉满。
- ResizeHandle 本身就是分隔，6dp 命中热区内只画一条线；hover/拖动只换 line_focus，
  不改变粗细。普通描边所有状态保持 kLineWidth，只切换语义颜色。
- 结构线为 line_subtle，项目状态栏上缘为 line_default；焦点环与活动下划线使用
  `kEmphasisLineWidth = 2.0F`。侧栏与内容区之间、标题栏下缘不另画线。
- 色阶只用于状态/控件/浮层：列表 hover/选中为 islands.raised/active，
  输入框与组合栏底、浮层、头像及媒体画布可有层次，普通分区底色不分色阶。
- 深色骨架是黑白灰：画布 #000000，表面 #121212/#202020，结构线基色 #666666，
  正文 #ffffff；浅色为 #ffffff/#f6f8fa/#d0d7de/#1f2328。
  品牌青只用于按钮、焦点环等交互状态；方法色与危险色按 UI 运行时引用各自取色。
- 页面不重复展示全局状态栏已有信息；k6 引擎就绪状态只在底部状态栏。
- Spacer 自带 Grow(1)，零尺寸占位用空 Row/Column。根链逐层有界：外壳 Column
  使用 CrossAlign(Stretch)，页面根 Grow(1.0F)，分区占满空间且内容在分区内滚动，
  不用 ScrollView 外套自包含分区。

菜单与弹窗的自适应宽度另见
[UI 运行时约束](../project-skills/huxerui-app-development/references/apitab-runtime.md)。
