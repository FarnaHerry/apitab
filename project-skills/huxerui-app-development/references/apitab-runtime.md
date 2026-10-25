# apitab UI 运行时与交互约束

本文件保存项目的挂载、预览、菜单与事件约束；对应修改前按小节阅读。

## UI 源码、主题与资源

- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；应用声明在 `src/app.cpp`，平台 `main()` 位于
  `platform/<platform>/main.cpp`，根 composable 在 `src/ui/app.cpp`。
- 主题从 MaterialTheme/MaterialDarkTheme 等内置主题定制；受控值以应用状态为
  权威；动态兄弟用稳定 Key。
- **Theme 是环境节点，不能附加 View 行为**：不要写 `Theme(...).With(Grow(...))`；
  Grow、Frame、Background、事件和过渡锚点应放在内部内容容器上。此类错误编译能通过，
  挂载时会抛 `HuxerUI Environment node cannot declare view behavior`；修改主题边界后
  必须覆盖实际挂载与交互回归（设置页见 `tests/test_ui_layout.cpp::OpenSettingsTheme`）。
- 框架资源：resources.bin 拷到 `<exe>.resources/huxerui/`（POST_BUILD 完成）。
- **WebView（响应 HTML 预览）**：`third_party/lib-webview`（FarnaHerry/Lib-WebView
  fork）经 `huxerui_use_library` 接入；`InstallWebView` 注册在
  `AppOptions::application_hooks`（应用级一次注册，与托盘激活同规）。Linux 后端
  WebKitGTK 6.0（缺 `webkitgtk-6.0` 开发包时整条降级）：CMake 探测结果落
  `APITAB_HAS_WEBVIEW` 宏，app.cpp 钩子与响应区 Preview 标签都以它门控。
  **hcg codegen 不支持 composable 函数体内的条件编译**（实证
  `conditional compilation inside a composable function is not supported`）：
  WebView 相关分支必须整体收在函数级 `#ifdef`（`request_response.cpp` 的
  `ResponsePreviewTabContent`/`AppendResponsePreviewTab` 双实现模式）。
  预览语义：HTML 原文经 `ResponsePreview`（html/base_url/revision）状态贯通，
  挂载后与每次新响应由 `Lifecycle` 重放 `LoadHtml`（WebView 是 PlatformView
  重资源，随档挂载/卸载不常驻）；主框架跳转一律取消（预览锚定本次响应），
  相对资源以请求 URL 为 base。

## 自适应菜单宽度

- **弹层菜单行禁用 `Grow`（菜单宽度必须自适应）**：流式布局测量中，行内只要
  有 Grow 子项且约束有限，行宽直接取约束上限、无视内容固有宽度（HuxerUI
  `containers.cpp` MeasureAxisLayout）；而弹层的测量上限≈视口宽——菜单行用
  Grow 推尾随图标曾导致所有菜单层（根层/级联子层）铺满可用宽度（6266d446
  引入，fff4b557 修复）。自绘菜单（common.cpp `PopupMenuContent` /
  `PopupMenuRow`）的宽度契约：整层收缩到最宽条目、下限取
  `MenuStyle.minimum_width`，不加宽度上限；尾随元素（级联箭头等）紧随文字
  排布（Spacing(6)），通栏 hover/选中背景靠外层 Column 的
  `CrossAlign(Stretch)` 保证。其他"容器宽度由内容决定"的弹层/自适应场景同理：
  行内 `Grow` 或 `MainAlign` 非 `Start` 都会 claim 可用上限，不得使用。

## 托盘与应用注册

- **托盘激活改「应用级一次性注册」（2026-09-24 上游 08acc36 `refactor(runtime):
  separate application and window ownership`，本仓 pin 0c51262 起生效）**：
  `SystemTrayHandle::OnActivate` 从**组合期订阅**变成**应用级主激活处理器**——
  旧签名 `OnActivate(handler, deps...)` 内部自己包 `Lifecycle`（可随依赖重连、
  每次重组调用都安全），新签名 `OnActivate(handler)` 只认一次注册、活到 Runtime
  关闭，**重复注册抛 `std::logic_error`**；`Show/Hide` 同步去掉 owner 参数。
  所以托盘激活的唯一注册点是 `AppOptions::application_hooks`
  （`src/app.cpp` 挂 `apitab::ui::InstallSystemTray`，`src/ui/app.cpp` 实现，
  经 `TrayWindowController` 定位窗口，窗口句柄在根挂载时写入），
  **绝不能写在会重组的 composable 里**：写在 AppRoot 里第二次重组即抛异常
  `std::terminate`（v0.1.11–v0.1.14 启动、托盘宿主就绪后闪退的根因）。上游契约见
  `third_party/huxerui/docs/design/system-tray.md`「Composed behavior」与
  `docs/guide/core-concepts.md`。

## 右键、选择器与主题切换

- **已用上的 2026-08-30 新能力**（已包含于 SDK 0.2.0；
  `PointerEvent button 字段`计划项已落地）：右键事件
  `ViewEvents::ContextMenuRequested` + `ShowPopupMenuAt`（请求树列表右键
  菜单，自绘 PopupMenu）；官方 `Select` 下拉（设置页/历史页/压测页方法，
  自绘 DropdownSelect 已删；组合栏内的方法/环境触发器因自带描边外观仍
  手拼——方法触发器用 `ShowPopupMenu`，环境触发器仍用 UseMenu）；`PointerEvent.changed_button/pressed_buttons` 区分
  左/右/中/前进/后退键；`SceneTransitionHandle.RunFromCurrentInteraction`
  （主题切换动画从点击处展开，设置页）。
- **0.2.0 画笔统一**（2026-09-01，f4f369a `feat(paint): unify color and
  gradient brushes`）：`Fill` 改为 `VisualFill = variant<Brush, ImageFill>`，
  `Brush = variant<Color, LinearGradient, RadialGradient>`；取纯色需两层
  下钻（common.cpp PopupMenu 选中底色即此模式）。渐变支持 path 填充/描边
  与变换；SVG 编译器新增渐变支持。本批未发 linux 预置 hcg/hrc，本地自编译
  宿主工具流程仍为必需（见上文）。
- **hover 已换官方 API**（2026-08-31，487eeff `feat(events): add hover
  lifecycle events`）：`ViewEvents::Hover`（Enter/Move/Leave，containment
  语义——绑定 View 的子组件间移动不发 Leave，只挂 Hover 不进 pointer
  route）。原 HoverTrack/HoverCell（ui.h 自研聚合扩展）已整体删除，行/标签
  悬停显隐 = 最外层容器挂一个 `.On<ViewEvents::Hover>`。NodeExtension 的
  hover 回调同步改名为 `OnHover(MountedNode&, const HoverEvent&)`。

## 弹窗环境与方法配色

- **弹窗层捕获调用处环境**：`dialog.Show` 的层内容继承组合处的
  Environment/Theme。AppRoot 在 MinimalThemed provider **之上**（自身
  UseTheme 只拿到默认浅色 spec），所以关闭询问弹窗等需要主题的层必须
  在 provider 之下的 composable 里 Show（关闭弹窗宿主 =
  app.cpp `CloseGuard`，包在 MinimalThemed 内）。自定义内容弹窗统一走
  `DialogCard`（common.cpp）包底板；布局统一：Column 加
  `CrossAlign(Stretch)` + 固定宽，按钮行 `MainAlign(SpaceBetween)`。
- **方法配色与危险颜色约定**（2026-08-31 起，两者**已分离**）：
  ① HTTP 方法统一色表 `MethodColor(theme, method)`（ui.h）：GET 绿 /
  POST 琥珀 / PUT 蓝 / PATCH 紫 / DELETE 玫红（**不是** error 红）/
  HEAD 灰蓝 / OPTIONS 青 / CONNECT 棕 / TRACE 灰 / QUERY 靛蓝 /
  PURGE 深橙，深浅主题各一套；WebDAV 长尾（PROPFIND 等）与 WS/TCP
  徽标回落 on_surface_variant。方法列表 20 个（经典 7 + CONNECT/
  TRACE/QUERY/PURGE + WebDAV 9 个；curl 走 CURLOPT_CUSTOMREQUEST、
  k6 走 http.request 都接受任意方法名；框架 HttpClient 的 HttpMethod
  枚举只有经典 7 个 → http_test_page 不扩充）。染色点 = 请求页徽标/
  标签 chip/拖影、MethodUrlBar 触发器与自绘下拉项、压测页方法
  Select（item 工厂按组合期快照着色，不按引用捕获 theme——工厂会被
  菜单层稍后调用）。
  ② **危险色 = `ThemeSpec::error`，专属危险操作**（删除/清空等），
  方法 UI 一律不用。自绘弹出菜单统一走
  `ShowPopupMenu`/`ShowPopupMenuAt`（common.cpp，UsePopup 承载，外观
  对齐环境 MenuStyle；作者口径：MenuItem 无 per-item 配色 API，需要
  定制就自己用 UsePopup 做菜单内容）。**选择列表选中项不用对钩，填充
  比 hover 深一档的底色**（取 item_indication.press 填充色，兜底
  surface_container_highest）。条目 `PopupMenuDanger`：
  kHoverRed = 常态普通色、hover 变红（集合树 ⋮/右键与编辑器 ⋮ 的
  "删除/删除分组"）；kAlwaysRed = 常驻红。`PopupMenuItem::label_color`
  可逐条自定义文字色（方法下拉即用它按 MethodColor 着色）。官方
  Select 下拉走 item 工厂：工厂里给单个 item 设
  `Indication{.hover = ...}` 会覆盖默认悬停色（作者口径）。
  **紧凑有限选项（如历史页“每页 10/20/50/100”）不能只给触发框加 `Frame`**：
  `Select` 弹层的最小宽度取 `max(SelectStyle::minimum_width, trigger_width)`。
  应从 `UseEnvironment<SelectStyle>()` 复制当前主题样式，局部 `Theme` 中把
  `minimum_width` 改为控件宽度，并对触发框使用同一 `Frame::width`；两处必须同步，
  以免触发框已收窄而菜单仍保留全局 120dp 宽度。常规选择器继续使用全局样式，不做
  无差别缩窄。
  ③ 确认弹窗里的危险操作按钮**直接显示危险色**：统一走
  `ShowDangerConfirm`（common.cpp，DialogCard + ProvideEnvironment 局部
  覆盖 ButtonStyle 为红底白字）；清空历史、删除环境、删除请求（集合树
  ⋮/右键菜单与编辑器 ⋮ 菜单两处入口）等破坏性确认均已迁入。
