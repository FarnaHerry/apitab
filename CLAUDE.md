# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

apitab 是一个 **C++23 模块化 GUI 工具**：API 测试（单次请求走 curl 引擎抽象
`api::ApiEngine`）+ k6 压测 + SQLite 存储，用 **CMake** 构建。前端为 **HuxerUI 0.2.0**（组件式声明 UI，源码/官方 SDK 双通道）；迁移地图见 `docs/huxerui-migration.md`。分层：UI（src/ui/*.cpp +
app_main.cpp，普通 C++ 源走 hcg codegen）/ 领域 store（apitab.* 模块）/ 引擎
三层，引擎自保留在 store 单例中。

## HuxerUI 开发参考

UI 工作先读官方 skill：`.claude/skills/huxerui-app-development/SKILL.md`
（从 HuxerUI 官方仓库同步：`cp -a third_party/huxerui/skills/huxerui-app-development/
.claude/skills/huxerui-app-development/`；references/ 含 dsl-style、components、
fundamentals、layout-and-ui、theme-animation-presentation、navigation-and-window
等分册）。注意三处 `apitab local addendum` 本地补充（重新同步上游后需
保留/重放）：`references/gestures-and-drag-drop.md` 的 Hover 节（hover
双通道语义与组合行配方）、`references/components.md` 的 Select 节
（item 工厂 per-item Indication 定制）、`references/theme-animation-
presentation.md` 的 Presentation services 节（UsePopup 自绘菜单配方）。
要点：

- HuxerUI 接入采用“**SDK 工程契约驱动、源码解析优先**”：CLI、项目自省、codegen、
  resource compiler 和打包继续遵守 HuxerUI SDK 工程格式，但开发构建优先
  `third_party/huxerui` 源码（git clone 的上游仓库，`add_subdirectory` 编译，跟踪
  上游更新 `git pull` 即可；不入库，.gitignore 已忽略）。CLI 注入的已安装 SDK
  `HUXERUI_HOME` 不得压过仓库源码；只有 `HUXERUI_HOME` 明确指向源码目录时优先于
  仓库源码。源码目录不存在或 Linux 缺 gtk4/libsoup-3.0 开发包时才自动回落
  `third_party/tarballs` 的 Linux 0.2.0 预编译 SDK（`find_package(HuxerUI 0.2.0 CONFIG REQUIRED
  COMPONENTS shared)`，`HuxerUI_DIR` 指向 `build/vendor/huxerui/...`）。
  源码模式编译 Linux 后端需要 `sudo dnf install gtk4-devel libsoup3-devel`。
  强制回落已安装/离线 SDK：`-DAPITAB_HUXERUI_FORCE_SDK=ON`；该通道是兼容与发布门禁，
  不阻塞开发期使用最新源码 API。
- 源码 clone 当前带本地补丁：`tools/codegen/CMakeLists.txt` 与
  `tools/resource_compiler/CMakeLists.txt`
  的 Linux 静态链接改为可用 `-DAPITAB_HOST_TOOLS_DYNAMIC=ON` 关掉（本机无
  libstdc++-static，免 sudo；仅影响本机自编译宿主工具）。
  `include/huxerui/window.h` 的 `WindowTitleBar` 构造函数里
  `CrossAlign(CrossAxisAlignment::Center)` 改为 `CrossAlign{...}`：函数式强转
  依赖 P0960，clang 在 Objective-C++ 下不实现，导致 macOS 所有 `*.mm` TU 编译
  失败；花括号聚合初始化跨语言通用。该补丁以
  `cmake/patches/huxerui-window-p0960.patch` 形式在 CI 两个 fetch 步骤里
  `git apply`（改动处带 `NOTE(apitab local patch)`，上游 pull 后需核对/重导，
  建议反馈作者改用花括号）。
  原 `platform/linux/linux_adapter.cpp` 的 X11 `None` 冲突补丁已由上游
  `dcd41c4` 正式修复，937efb1 更新后本地补丁已删除。
- **上游 linux 预置宿主工具可能落后于源码**：4a56daf 改了 svg 编译器/path
  编码却只重发了 mac/windows/android 的预置 hrc/hcg，linux 预置仍是旧的
  → 新运行时解旧资源包启动即抛 "unknown path operation"。对策：用当前
  源码本地构建 hcg/hrc 到 `third_party/huxerui-tools/linux/x86_64/`
  （已 gitignore）：
  `cmake -S third_party/huxerui/tools/<codegen|resource_compiler> -B
  build/host-tools-build/<...> -G Ninja -DCMAKE_BUILD_TYPE=Release
  -DAPITAB_HOST_TOOLS_DYNAMIC=ON && cmake --build ... && cmake --install ...
  --strip --prefix third_party/huxerui-tools/linux/x86_64`；
  顶层 CMakeLists 检测到该目录即设 `HUXERUI_HOST_TOOL_ROOT` 优先使用。
  **每次 pull 后都应重跑一次该流程**（并删除 build/hcg、
  build/huxerui-resources 等旧工具产物缓存）。上游补齐 linux 预置后可删。
- sweetedit 接入：`third_party/huxerui-sweetedit`（git clone，不入库，已
  gitignore）提供 SweetEditor 代码编辑器（行号/语法高亮），经
  `huxerui_use_library(apitab TARGET sweetedit_core ...)` 编译；其
  3dparty/SweetEditor、SweetLine 是 gitlink 但上游无 .gitmodules，真实源为
  github.com/FinalScave/SweetEditor(bd4330e)、FinalScave/SweetLine(64665d9)，
  已手动 clone 固定提交。已用于请求页 body 编辑器（request_page.cpp
  BodyTextEditor）和压测页 k6 脚本编辑器（loadtest_page.cpp）；语法 DSL 在
  src/ui/syntax_grammars.h（kJsonSyntax/kJavaScriptSyntax/kXmlSyntax/
  kGraphqlSyntax/kPlainSyntax）。body 文本按类型独立存档（RequestDraft::bodies
  下标 = api::BodyKind 值，保存时全量落 db::SavedRequest::bodyContents）。
  注意：SweetEditor **非受控组件**，外部改文本必须
  `controller.LoadDocument(key, text, syntax)`，仅 document_key 变化才重载
  initial_text。本地补丁（改动处均带 `NOTE(apitab local patch)` 注释，
  **上游 pull 后需逐一核对/归位**）：① `include/sweetedit_core/sweet_editor.h`
  的 11 个事件声明从旧式 `Event<Args...>`（面向早期 SDK）改为 HuxerUI
  main 的函数签名式 `Event<void(Args...)>`，发射侧不受影响；②
  `src/sweet_editor/sweet_editor.cpp` 的 8 处 StrokePath/DrawBorder 由
  float 线宽改为 `StrokeStyle{.width=...}`、`Extension::OnKey` 返回类型
  void→bool（跟进 81e8bbc 统一描边样式、a7d38c9 可消费键盘路由）；
  ③ **调色板注入**：新增 `SweetEditorPalette`（约 60 个 ARGB 字段，
  默认=原浅色）+ `SweetEditorOptions::palette`，sweet_editor.cpp /
  sweetline_highlighter.cpp 全部颜色（含语法高亮与彩虹括号）改读 palette。
  所有 `0x........` ARGB 字面量一律包 `static_cast<int32_t>(...)`：MSVC
  `/std:c++latest` 按 C++26 规则把常量表达式里的非值保留整型转换判为窄化
  （error C2397），windows-x86_64 CI 实证编译失败。
  应用侧经 `EditorPalette(theme)`（common.cpp，深色=结构色从 ThemeSpec
  派生 + VS Code Dark+ 语法色）在 BodyTextEditor 与压测页脚本编辑器接线。
  ④ `sweet_editor.cpp` 的滚轮桥接通过 CMake 头文件特性检测兼容旧 `ScrollEvent`
  与 937efb1 新 `ScrollInputEvent`（嵌套滚动/overscroll 统一；delta 字段语义不变），
  保持源码 main 与 0.2.0 SDK 双通道可编译。
  均建议反馈作者：sweetedit 需跟进 HuxerUI main 的 Event/Stroke/键盘
  API 变化，并支持官方主题配色入口。
- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；入口/根写在 src/app_main.cpp + src/ui/app.cpp。
- 主题从 MaterialTheme/MaterialDarkTheme 等内置主题定制；受控值以应用状态为
  权威；动态兄弟用稳定 Key。
- 框架资源：resources.bin 拷到 `<exe>.resources/huxerui/`（POST_BUILD 完成）。
- **SDK/源码版本更新时检索计划项**：HuxerUI 版本变化（tarball 换新或
  `third_party/huxerui` git pull 升级）后，按 `docs/plans/` 里各计划的
  「版本检索规则」检索新版能力（多窗口/跨窗口拖放等，见
  `docs/plans/project-tab-tear-off-window.md`）；不更新则无需检索。
- **源码 main 937efb1 新能力（2026-09-02）**：新增受控可编辑 `ComboBox`
  （自由输入 + 应用提供候选，`OnChanged` 与 `OnSelected` 分离）、Snackbar，以及
  嵌套滚动/overscroll 统一。本仓 0.2.0 预编译 SDK 尚不含这些 main 能力；开发期
  默认源码通道可以使用，合并/发布前必须刷新 SDK 并完成三通道验证。应用场景与边界见
  `docs/plans/island-structure-theme.md` §十四。
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
  ③ 确认弹窗里的危险操作按钮**直接显示危险色**：统一走
  `ShowDangerConfirm`（common.cpp，DialogCard + ProvideEnvironment 局部
  覆盖 ButtonStyle 为红底白字）；清空历史、删除环境、删除请求（集合树
  ⋮/右键菜单与编辑器 ⋮ 菜单两处入口）等破坏性确认均已迁入。

## 构建 / 运行

```bash
huxerui run linux                  # HuxerUI CLI 流程：构建到 .huxerui/build/linux/ 并运行
huxerui build linux --profile release
cmake -B build -G Ninja            # 直接 CMake 流程（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译
ctest --test-dir build             # 冒烟测试（test_smoke）
./run.sh                           # 启动 GUI（切到仓库根 + INTEL_FORCE_PROBE=1）
```

- 项目结构对齐 `huxerui create app` 生成格式：顶部 plan 自省块（CLI
  识别项目的依据）、`platform/<平台>/main.cpp` 平台入口（`main()` 里先
  `loadSessionPreferences()` 再 `RunApplication()`）、`src/app.cpp` 只持有
  `Application` 单例 + `requestUiUpdate` no-op 钩子、src/ 递归 glob 源码、
  resources/ 以 app 命名空间注册。CLI 在 `~/.local/bin/huxerui`，应软链到
  `~/.local/share/HuxerUI/bin/huxerui` 的当前安装 SDK。
- CLI 的库图扫描模式（HUXERUI_LIBRARY_GRAPH_ONLY）下不启用语言、不构建依赖，
  顶层 CMakeLists 的所有重活都以该变量为门控跳过。

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 4.4（`import std` 仍是
  experimental：版本相关 UUID 表在 `cmake/CxxImportStdGate.cmake`）。
- **依赖全部 vendor 在 `third_party/`**（tarball + SHA256，configure 期解包到
  `build/vendor/`，离线可复现；版本与原 mcpp.lock 一致，清单见
  `third_party/README.md`）：HuxerUI（**优先 `third_party/huxerui` 源码
  clone 编译**，或由 `HUXERUI_HOME` 选择已安装 0.2.0 SDK；Linux 离线回落同为 0.2.0）、
  Asio 1.38.1（`import asio` 模块在 `cmake/asio.cppm`）、IXWebSocket 12.0.1
  （client-only、无 TLS/zlib）、SQLiteCpp 3.3.3
  （内置 amalgamation）、nlohmann::json 3.12.0（`import nlohmann.json` 模块在
  `cmake/nlohmann.json.cppm`）。单次 HTTP 走 curl 引擎抽象 `api::ApiEngine`
  （`apitab.curl_engine`，常驻工作线程实现），由领域 store 持有，便于替换。
- OpenSSL：优先系统包；linux x86_64 找不到系统 OpenSSL 时回落到 vendor 的静态
  3.5.1（`third_party/tarballs/openssl-3.5.1-linux-x86_64.tar.gz`）。本机缺 X11
  开发头时，构建会借用 mcpp subos 视图的 X11/GL 子集（`third_party/CMakeLists.txt`
  兜底分支）；有 sudo 时直接 `dnf install libXrandr-devel libXcursor-devel
  libXinerama-devel libXi-devel` 更干净。
- **k6 不在本仓库**：CI 打包时下载对应平台二进制放 `engines/` 随包分发；运行时按
  `<exe>/engines/k6 → <exe>/k6 → <repo>/engines/k6（开发形态）→ PATH` 解析
  （`src/config.cppm::cfg::k6Binary()`）。`engines/` 已 gitignore。

## 架构

全模块化（`import std` + 各 `apitab.*` 模块），UI 只面向引擎抽象：

| 模块 | 文件 | 职责 |
|------|------|------|
| `apitab.api_engine` | `src/api_engine.cppm` | 抽象接口 `ApiEngine` / `LoadEngine` / `WebSocketEngine` / `TcpEngine` + `RequestSpec` / `ResponseView` / `LoadOptions` / `LoadSummary` |
| `apitab.curl_engine` | `src/curl_engine.cppm/.cpp` | 单次请求引擎的 curl 实现（常驻工作线程；send 纯入队、代际丢弃、cancel 协作打断且丢弃排队请求、取消后结果不投递；run 全 try/catch 兜底填错误结果）；`makeCurlEngine()` 工厂，curl 头只进实现单元 |
| `apitab.k6_engine` | `src/k6_engine.cppm/.cpp` | 压测引擎：生成 k6 脚本（`handleSummary` 打印 `K6SUMMARY {json}` 行）→ spawn 子进程 → 监视线程拆 `\r`/`\n` 行入队；stop=SIGINT，3s 宽限后 SIGKILL |
| `apitab.db` | `src/db.cppm/.cpp` | SQLiteCpp：requests / history / load_tests 三表；KV 序列化为 JSON |
| `apitab.config` | `src/config.cppm` | 数据目录（~/.local/share/apitab）/ k6 二进制解析 |
| `apitab.utils` | `src/utils.cppm` | 纯 string/number 帮助函数 + percentEncode / appendQuery |
| `apitab.store.requests` | `src/store/requests.cppm` | 领域 store：`g_requests` 持有 curl 引擎与 Db；组织/项目/集合 CRUD / finalizeSpec（{{var}} 环境变量替换 + 拼 URL + 合并全局 Cookie）/ sendViaEngine / takeResponse / recordHistory（落历史） |
| `apitab.store.loadtest` | `src/store/loadtest.cppm` | 领域 store：`g_loadtest` 持有 k6 引擎；start/stop/drainOutput/pollSummary（落压测记录） |
| `apitab.preferences` | `src/preferences.cppm` | 会话偏好（settings.ini 的 session.*），跨会话状态恢复 |
| `apitab.ui.*`（普通 C++） | `src/ui/*.cpp` | HuxerUI 前端：app（导航壳）/ home_page / request_page / common；ws_session/tcp_session（WS/TCP 会话，页面协程持有）、task_bridge.h（线程协程桥） |
| `src/app_main.cpp` | 普通 TU | 入口：`Application{AppRoot, AppOptions}` + `RunApplication()`；`requestUiUpdate` no-op 钩子 |

**入口**：`huxerui_add_app(apitab SOURCES ...)` 生成 app 目标并启用 hcg codegen；
领域层模块经 FILE_SET 追加到同一目标，`main()` 在 `src/app_main.cpp`。

**事件驱动**：HuxerUI 是 State 驱动失效模型。UI 线程 ↔ 任务线程的分离由
`src/ui/task_bridge.h` 的协程桥承担：单次 HTTP 请求由 store 持有的 curl 引擎
传输（send 纯入队，UI 协程 `PollWhile` 轮询 `takeResponse` 取回结果，恢复点
恒为 UI 线程）；WS/TCP 会话由
页面协程直接持有（`ws_session`/`tcp_session`，应用侧不拥有线程：IX 自管线程，
TCP 全同步 asio 经 `RunOnTaskThread` 上任务线程）；k6 引擎异步结果经
`PollWhile` 轮询 `pollSummary/drainOutput` 并在 UI 线程写 State；阻塞/CPU
重活经 `RunOnTaskThread` 派到任务线程池。旧 `requestUiUpdate` 唤醒钩子保留
为 no-op（仅 k6 引擎仍引用）。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统/第三方头放全局模块片段
   （`module;` 与 `module apitab.x;` 之间）。反过来，**普通 UI .cpp / ui.h 头**
   （非模块 TU）用哪个 std 设施就得自己 `#include` 哪个——libstdc++ 会传递性带入、
   CI linux 的 libc++ 不会（ui.h 用 `std::from_chars` 缺 `<charconv>` 实证炸过）。
2. **跨模块前向声明（如 `core::platform::requestUiUpdate`）必须放全局模块片段**——
   写在模块域内会被附加模块名修饰（`@apitab.xxx`），链接不到外部符号。
3. **UI 层遵守官方 skill 的 DSL 风格**（`.claude/skills/huxerui-app-development/
   references/dsl-style.md`）：普通 .cpp、composable 不加 inline、View 按值传递。
4. **asio 一律经 `import asio;`**（`cmake/asio.cppm`，SSL 由 `APITAB_ASIO_SSL` 门控）。
   不要在全局片段 `#include` asio 头：模块 BMI 与头文件各自的内部链接静态对象
   （prefer/require/query 的 static_instance）在 GCC 下会重复定义。
5. **受控值以应用状态为权威**（官方 skill：controlled values）；TextField 保留
   完整 TextEditingValue；动态兄弟用稳定 `.Key(...)`。
6. **异步结果**：线程契约与协程桥在 `src/ui/task_bridge.h`——State 只在 UI
   线程读写；引擎结果用 `PollWhile(interval, tick)` 按节拍取回（tick 内
   drain/poll + 写 State）；阻塞/CPU 重活用 `co_await RunOnTaskThread(fn)`
   派给任务线程池，结果/异常回 UI 线程恢复。`requestUiUpdate` 是 no-op
   钩子（app_main.cpp），不要新加调用。
   **事件处理器内禁止同步写会导致点击节点被卸载的 State**（如切页/关标签/
   切主题）——pointer-up 处理中同步重组会卸载按钮子树，框架随后 erase
   PointerSession 段错误。必须经 `tasks.Launch` + `co_await Delay(0)` 推迟；
   组合体内也不要写 State（挂载路径重入），初始值在 UseState 之前算好。
7. **k6 指标**：Trend 汇总默认只带 avg/min/med/max/p(90)/p(95)——脚本 options 里
   已声明 `summaryTrendStats` 加 p(99)，p50 用 `med` 键（没有 `p(50)`）。
8. **Spacer 自带 Grow(1)**：零宽/零高占位绝不能用 `Spacer().With(Frame{...})`——
   它会和兄弟平分剩余空间（曾把首页挤到右半屏）。占位用空 `Row{}`/`Column{}`；
   岛屿布局的根链必须层层有界：外壳根 Column 要 `CrossAlign(Stretch)`，页面根
   `Grow(1.0F)`，岛占满分区块、内容在岛内滚动（不要 ScrollView 套自包含岛）。

## CMake 迁移备注（原 mcpp 行为对照）

- Windows：HuxerUI 运行时 dll 在 POST_BUILD 拷到 exe 旁；OpenSSL 走 chocolatey，
  CI 里以 `OPENSSL_ROOT_DIR` 注入。
- CI 的 linux job 用 ubuntu:26.04 容器 + clang-21/libc++（详见
  `.github/workflows/build.yml` 注释）。- 版本号唯一来源是顶层 `project(apitab VERSION ...)`；packaging 脚本与 NSIS 从
  CMakeLists 解析。

## CLI（agent 可用）

无 GUI 的命令行模式：`./build/apitab --cli <子命令> [参数]`（`help`/`orgs`/`projects`/
`requests`/`show`/`send [--json]`/`history`）。与 GUI 共用 `~/.local/share/apitab` 的
SQLite 与 settings.ini，同一套项目/组织/环境上下文；headless 可读状态、发请求。
完整用法、退出码约定、`--json` 输出形状与实现备注见
`docs/apitab-cli.md`（AI 批量操作前先读）。
