# 源码、SDK 与宿主工具

本文件承接根指引的依赖契约；版本与 pin 为迁移时记录，更新时以实际构建文件为准。

## 构建选择与 fork

- HuxerUI 接入采用“**SDK 工程契约驱动、源码解析优先**”：CLI、项目自省、codegen、
  resource compiler 和打包继续遵守 HuxerUI SDK 工程格式，但开发构建优先
  `third_party/huxerui` 源码（git clone 的上游仓库，`add_subdirectory` 编译，跟踪
  上游更新 `git pull` 即可；不入库，.gitignore 已忽略）。CLI 注入的已安装 SDK
  `HUXERUI_HOME` 不得压过仓库源码；只有 `HUXERUI_HOME` 明确指向源码目录时优先于
  仓库源码。源码目录不存在或 Linux 缺 GTK ≥4.14 / libepoxy ≥1.5 / libsoup ≥3.0 开发包时才自动回落
  `third_party/tarballs` 的 Linux 0.3.0 预编译 SDK（`find_package(HuxerUI 0.3.0 CONFIG REQUIRED
  COMPONENTS shared)`，`HuxerUI_DIR` 指向 `build/vendor/huxerui/...`）。
  源码模式编译 Linux 后端需要 `sudo dnf install gtk4-devel libepoxy-devel libsoup3-devel`。
  强制回落已安装/离线 SDK：`-DAPITAB_HUXERUI_FORCE_SDK=ON`；该通道是兼容与发布门禁，
  不阻塞开发期使用最新源码 API。
- 源码 clone 现为 **FarnaHerry/HuxerUI fork 的 farna/main**（CI 固定
  `a559c7880daa1402a23357ed049d2944428a9b44`，2026-11-10；包含 Linux PlatformView
  宿主支持、应用级多窗口与每窗口系统深浅色 hook，已合入上游 SDK 0.4.0：
  自定义字体族/主题排版、Task 回调集成、原生文件引用工厂）。`OpenWindow` 必须传独立
  `ViewFactory`；`b083028` 移除 `WindowHandle::Retire()`，统一用会经过
  `OnCloseRequest` 的 `Close()`；`83617e6` 再移除 `AppOptions::extra_windows`，
  额外启动窗口一律用 `OpenWindow`。`11161a7` 合入上游 Android window-scoped Java
  modules / Activity Result routing 与 iOS application/window-scoped Objective-C
  module factory（iOS factory 改名，仅移动端扩展受影响）。`97d6fed1` 合入上游
  受控分栏/停靠布局（`SplitLayout`/`DockLayout`/`DockTree`/`DockItem`，见
  `docs/design/split-and-docking.md`；另新增 `linux/window.h` 原生窗口几何头，
  apitab 未使用）。`49faac65` 为 TextField 尾部光标可见性修复 + 头文件外观清理；
  `3bea96ce` 合入上游 `TextField::TextStyles`（attributed character styles）、
  原生段落几何与手动软键盘控制（均纯增量）。`a559c788` 合入上游：Linux UI 任务
  饿死修复、程序化焦点（`FocusRequester`）与对话框初始焦点、`ShowKeyboard()/
  HideKeyboard()` 软键盘控制、随机访问文件流/同步文件引用 I/O、外部纹理帧获取；
  宿主侧文本布局虚函数改形（`CreatePlatformTextLayout`/`TextLayoutBackend`，
  apitab 非平台宿主不受影响）。无窗常驻须设 `quit_on_last_window_closed=false`，
  普通退出显式调用 `ApplicationHandle::Quit()`。系统主题用
  `UseSystemThemeBrightness()`，不再由 apitab 查询各平台设置。**历史本地补丁已
  全部被 fork 收编并删除**，不要恢复旧 API 别名或兼容层。
  WebView 组件库 clone：`third_party/lib-webview`（FarnaHerry/Lib-WebView 的
  farna/main，上游 HuxerUI/Lib-WebView + Linux WebKitGTK 6.0 后端等应用补丁；
  CI pin `09c820a1d8d10237148b3e1d103beaacdcee0034`，2026-11-10，含上游
  cookie 管理与状态读取整理，公开面未变 apitab 仍只用 `LoadHtml`）。
  **刻意 pin 在删前状态**：fork tip `c0391ac` 随官方 SDK 0.4.0 迁移（作者
  `1deb796`）删除了 Linux WebKitGTK 后端（官方 SDK 已无 PlatformView 工厂），
  而本仓跟的 HuxerUI fork 仍保留 `linux/platform_registry.h` 工厂；fork 侧恢复
  后端后再跟进 tip。不入库、已 gitignore，经 `huxerui_use_library(apitab
  TARGET HuxerUI::WebView PATH ...)` 接入。
  Markdown 组件库 clone：`third_party/lib-markdown`（HuxerUI/Lib-Markdown
  **v0.3.0**，CI pin `247ed949f571c186aeffb1d57db4c4b7407538e0`，2026-11-10；
  破坏性裁剪版：作者把富文本/源码编辑整块（`MarkdownEditor`、`editing.h`、
  source ranges 等）移入 `archive/full-rich-text` 分支，库只保留解析 + 只读渲染；
  apitab 所用 `ParseMarkdown`/`Markdown`/`MarkdownStyle::FromTheme`/
  `MarkdownEvents::LinkActivated` 全部保留，无需改码，0.3 起也不再依赖
  `TextField::TextStyles`）。MD4C 扩展（highlight/insertions/underline/
  wiki_links/admonitions/footnotes/math/preserve_blank_lines/spoilers 等）
  默认全关。不入库、经 `huxerui_use_library(apitab TARGET HuxerUI::Markdown
  PATH ...)` 接入。`ParseOptions::autolinks` 自 v0.2.1 起为
  `MarkdownAutolinkOptions` 聚合体（默认行为不变）；apitab 用默认 ParseOptions。
  该库的 `MarkdownStyle::FromTheme` 依赖 `Font::FromFamily` 与
  `theme.typography.family`（0.4.0 系列），**离线 `huxerui-sdk-0.3.0` 没有**：
  CMake 读实际 `text.h`/`theme.h` 检测，不满足时整库不接入并输出降级说明，
  应用侧以 `APITAB_HAS_MARKDOWN` 门控（显示等宽原文）。
- **上游预置宿主工具已随源码刷新**（95f07ab 起由 CI 自动重发 linux 预置 hcg/hrc），
  旧「4a56daf svg/path 编码 linux 预置落后」问题与本地
  `third_party/huxerui-tools` 构建流程已删。若未来预置再次落后，临时对策仍是
  用当前源码本地构建 hcg/hrc 并设 `HUXERUI_HOST_TOOL_ROOT`；**每次 pull 后删除
  build/hcg、build/huxerui-resources 等旧工具产物缓存**再配置。

## 更新后检索计划

- **SDK/源码版本更新时检索计划项**：HuxerUI 版本变化（tarball 换新或
  `third_party/huxerui` git pull 升级）后，按 `docs/plans/` 里各计划的
  「版本检索规则」检索新版能力（多窗口/跨窗口拖放等，见
  `docs/plans/project-tab-tear-off-window.md`）；不更新则无需检索。

## 能力检测与双通道

- **源码 main 937efb1 新能力（2026-09-02）**：新增受控可编辑 `ComboBox`
  （自由输入 + 应用提供候选，`OnChanged` 与 `OnSelected` 分离）、Snackbar，以及
  嵌套滚动/overscroll 统一。当前构建按 SDK 实际头文件检测 ComboBox 等 API，并在源码
  与离线 SDK 通道选择兼容实现；CMake configure 输出会报告检测结果。新增 HuxerUI API
  后仍需在源码与发布 SDK 通道验证。应用场景与边界见 `docs/plans/island-structure-theme.md` §十四。

## 工程与工具链

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
  clone 编译**，或由 `HUXERUI_HOME` 选择已安装 0.3.0 SDK；Linux 离线回落同为 0.3.0）、
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

## CLI 开发构建

```bash
huxerui run linux
huxerui build linux --profile release
```

CLI 构建产物在 `.huxerui/build/linux/`；直接 CMake 构建产物在 `build/`。
两条流程都保留 SDK 工程契约；完整应用编译和链接门禁仍按根指引执行。
