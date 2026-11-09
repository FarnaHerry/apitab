# third_party — vendored 依赖

从 mcpp 迁移到 CMake 后，原本由 mcpp 注册表（`~/.mcpp/registry/data/xpkgs/`）提供
的第三方库全部 vendor 进本仓库：tarball 提交在 `tarballs/`（configure 期校验
SHA256 后解包到 `build/vendor/`，源码树不入库），nlohmann::json 是 single header
直接提交在 `json/`。构建完全离线、可复现。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.3.0 | `huxerui-sdk-0.3.0-linux-x86_64.tar.gz` | 官方 0.3.0 SDK（shared 库 + headers + CMake 包 + huxerui CLI + 内置资源）。`HUXERUI_HOME` 可指向 0.3.0 SDK 安装目录或源码根目录；未设置时优先 `third_party/huxerui/` 源码，`APITAB_HUXERUI_FORCE_SDK=ON` 时使用 Linux 离线包。Linux 源码模式需 GTK ≥4.14、libepoxy ≥1.5、libsoup ≥3.0（Fedora：`gtk4-devel libepoxy-devel libsoup3-devel`）；macOS/Windows 必须通过 `HUXERUI_HOME` 提供 0.3.0 源码或 SDK。 |
| Asio | 1.38.2 | `asio-1.38.2.tar.gz` | 上游 `chriskohlhoff/asio` tag asio-1-38-2，同 `chriskohlhoff.asio` |
| IXWebSocket | 12.0.1 | `ixwebsocket-12.0.1.tar.gz` | 上游 `machinezone/IXWebSocket` v12.0.1，同 `compat.websocket`（client-only、无 TLS/无 zlib，32 个源文件） |
| curl | 8.22.0 | `curl-8.22.0.tar.gz` | 上游 `curl/curl` release tarball，同 `compat.curl` |
| SQLiteCpp | 3.3.3 | `SQLiteCpp-3.3.3.tar.gz` | 上游 `SRombauts/SQLiteCpp` v3.3.3（内置 sqlite3 amalgamation），同 `compat.sqlitecpp` |
| OpenSSL | 3.5.1 | `openssl-3.5.1-linux-x86_64.tar.gz` | `compat.openssl` 管线构建的**静态预编译产物**（libssl.a/libcrypto.a + include），仅 linux x86_64 兜底；其他平台用系统 OpenSSL（ubuntu libssl-dev / macos brew openssl@3 / windows vcpkg） |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include` |

## 组件库 clone（不入库）

以下 HuxerUI 组件库以 git clone 形式放在 `third_party/<名>/`，`.gitignore` 已忽略，
CI 按固定 commit 检出；接口经 `huxerui_use_library(... PATH ...)` 接入。

| 库 | 位置 | pin（2026-11-10） | 用途 |
|----|------|------------------|------|
| SweetEditor（Lib-CodeEditor fork） | `third_party/huxerui-sweetedit/` | `a332e7c`（FarnaHerry fork，apitab 补丁已全部收编） | Body/k6 脚本编辑器 |
| Lib-WebView | `third_party/lib-webview/` | `09c820a`（FarnaHerry fork，保留 Linux WebKitGTK 后端的最后提交） | 响应 HTML 预览 |
| Lib-Markdown | `third_party/lib-markdown/` | `247ed94`（v0.3.0，只读渲染裁剪版） | 请求文档 Markdown 渲染；依赖 `Font::FromFamily`，缺失时 CMake 不接入并以 `APITAB_HAS_MARKDOWN` 门控降级 |

## 为什么提交预编译的 OpenSSL

本机（Fedora）没有 openssl-devel 且 `import std` 的构建无法依赖 sudo 装包；
asio::ssl 与 curl https 又必须链接 OpenSSL。因此把原 compat.openssl 管线产出的
静态库原样收进 tarball，仅作为"系统找不到 OpenSSL 时"的 linux x86_64 兜底
（`third_party/CMakeLists.txt` 顶部的查找顺序）。若你愿意装系统包，可直接删掉
这个 tarball。

## 更新某个依赖

1. 用新版本源码打 tarball（保持顶层目录名，或同步改 `third_party/CMakeLists.txt`
   里 `apitab_extract` 的 `topdir` 参数）；
2. `sha256sum` 新值写回 `third_party/CMakeLists.txt`；
3. 跑一次 configure 验证解包与 SHA 校验。

### 2026-09-05 控件更新

- HuxerUI 已合并至 `371072b`，sweetedit 保持 `d012eb7`，原本地补丁保留；节点扩展
  已随上游破坏性变更统一使用 `ViewNode`（不保留 `MountedNode` 兼容名）。
- sweetedit `ContextMenuEntry::label` 改为拥有字符串，修复自定义菜单构建回调返回后
  `item.label.c_str()` 悬空的问题；补丁见 `cmake/patches/sweetedit-context-menu-label.patch`。

### 2026-09-10 第三方上游更新

- HuxerUI 更新至 `065ab79`（v0.3.0 系列），包含应用目录/剪贴板统一通过
  `ApplicationHandle` 访问、共享转场、UI 测试、mcpp 构建前端及测试/打包链路重构。
- SweetEdit 保持 `21925a2`，其上游已包含 `ViewNode` 和新版选择/焦点接口；本地滚轮
  事件与处理器适配仍通过 `cmake/patches/huxerui-sweetedit-*.patch` 维护。
- 与 HuxerUI/SweetEdit 上游冲突的本地窗口缩放、工具链及编辑器适配均只保留为补丁，
  不写入第三方上游历史。

### 2026-09-11 HuxerUI 0.3.0 SDK

- HuxerUI 源码跟随官方 `v0.3.0`（`d1d2daa`），本地兼容补丁继续单独维护。
- Linux x86_64 使用官方 `huxerui-sdk-0.3.0-linux-x86_64.tar.gz` 作为仓库离线 SDK 兜底包；默认构建仍优先使用源码。

### 2026-09-13 上游同步

- HuxerUI 源码 `d1d2daa` → `445488a`（Windows GUI 启动去控制台修复，仅
  `cmake/HuxerUIApp.cmake`、模板与文档；Linux 侧接口与库源码无变化）。三份本地补丁
  （resize-hitarea、window-p0960、hosttools-dynamic）经反向校验后重置重放，全部干净
  套用且不被上游覆盖，继续保留。
- SweetEdit 与嵌套 SweetEditor（`bd4330e`）、SweetLine（`64665d9`）上游均无新提交。
- 按流程重建 hcg/hrc 到 `third_party/huxerui-tools/linux/x86_64/`，并清理
  `build/hcg`、`build/huxerui-resources` 旧产物；`apitab` 全量构建链接通过。

### 2026-09-13 上游同步（PR #137）

- HuxerUI 源码 `445488a` → `64264cb`：合入本地贡献的 PR #137（Linux Pango 文本
  布局跨 measure 缓存与超限回收，仅 `platform/linux/linux_renderer.cpp` 与渲染
  测试），与三份本地补丁零重叠，反向校验后重置重放全部干净套用。
- SweetEdit 与嵌套 SweetEditor（`bd4330e`）、SweetLine（`64665d9`）上游仍无新提交。
- 重建 hcg/hrc；清掉 `build/huxerui-resources` 后 ninja 仍引用其中旧资源计划，
  重新 configure 再生成后 `apitab` 全量构建链接通过。

### 2026-10-29 HuxerUI fork 同步

- HuxerUI 源码 `5873eeb` → `b083028`：加入每窗口 `UseSystemThemeBrightness()`，
  主/子窗口跟随系统主题改为框架响应式 hook，删除 apitab 自写平台查询。
- `WindowHandle::Retire()` 被移除，拆窗关闭与轻量模式统一迁移到 `Close()`；
  `quit_on_last_window_closed=false` 保持无窗 Runtime，普通退出显式调用 `Quit()`。

### 2026-11-05 HuxerUI fork 同步

- HuxerUI 源码 `83617e6` → `11161a7`：合入上游 Android window-scoped Java modules
  与 Activity Result routing、iOS application/window-scoped Objective-C module
  factory（iOS factory 改名），以及 runtime dependency 暂存树冲突裁决修正。
  桌面 `app.h`/`window.h`/`gesture.h` 无差异，apitab 源码无需改动。
- `build` 全量构建链接通过，`ctest` 9/9 通过（含 41 s 的 huxerui_selection_matrix）。

### 2026-11-05 Lib-Markdown 接入

- 新增 `third_party/lib-markdown`（HuxerUI/Lib-Markdown `main`，pin `794f2c1`，
  含 0.2 API、`VirtualizedMarkdown`/UTF-8 流式解析、MD4C 扩展与实验性编辑会话），
  经 `huxerui_use_library` 接入；内置 md4c 为 C99，按库自身 CMake 编译。
  扩展（`highlight`/`insertions`/`underline`/`wiki_links`/`admonitions`/`footnotes`/
  `math`/`preserve_blank_lines`）默认关闭，应用侧未启用；需求出现时经 `ParseOptions`
  显式打开。
- 库的 `MarkdownStyle::FromTheme` 依赖 `Font::FromFamily` + `theme.typography.family`
  （0.4.0 系列），仓库离线 `huxerui-sdk-0.3.0` 没有；CMake 按实际头文件检测，
  不满足时整个库不接入，应用侧以 `APITAB_HAS_MARKDOWN` 门控降级为等宽原文。
- 应用侧用法：请求页「文档」分区渲染 `requests.doc`；导入 OpenAPI/Swagger/Postman
  时把 operation description 写入该列。

### 2026-11-06 HuxerUI fork 同步（Split/Dock 布局）

- HuxerUI 源码 `11161a7` → `97d6fed1`：合入上游受控分栏与停靠工作区
  （`SplitLayout`/`DockLayout`/`DockTree`/`DockItem`，`docs/design/split-and-docking.md`）、
  `linux/window.h` 原生窗口几何头与相机扩展的原生表面工厂。`app.h`/`window.h`/
  `gesture.h` 无 API 变化。
- 请求页工作区改用 `DockLayout`（`APITAB_HAS_DOCK_LAYOUT` 头文件检测门控）：
  面板拓扑/尺寸/拖拽停靠由框架管理；离线 0.3.0 SDK 回退到自管
  `SplitPane` + `ResizeHandle`（行为不变，仅 SDK 通道编译）。

### 2026-11-07 fork 与 Lib-Markdown 同步（3bea96ce / v0.2.2）

- HuxerUI 源码 `97d6fed1` → `3bea96ce`：TextField 尾部光标可见性修复（PR #152）、
  头文件外观清理，合入上游 `TextField::TextStyles`（attributed character
  styles）、原生段落几何与手动软键盘控制；`app.h`/`window.h`/`gesture.h` 无 API
  删除，apitab 源码无需改动。
- Lib-Markdown `794f2c1` → `6cd2efe`（v0.2.2 标签）：新增原生块编辑器
  `MarkdownEditor`（依赖 fork `3bea96ce` 的 `TextField::TextStyles`）、spoiler
  扩展、`MarkdownSourceRange`/`source_ranges`、`MarkdownAutolinkOptions`（原
  `bool autolinks` 变聚合体，默认值不变）与 Writer 演示应用。apitab 使用默认
  ParseOptions 渲染路径，无影响。

### 2026-11-10 全 fork 追新

- HuxerUI 源码 `3bea96ce` → `a559c788`：合入上游 Linux UI 任务饿死修复、程序化
  焦点与对话框初始焦点、随机访问文件流/同步文件引用 I/O、外部纹理帧获取、
  Windows GUID 导出修复等；fork 的 Linux PlatformView 工厂 API 原样保留。
- Lib-Markdown `6cd2efe` → `247ed94`（**v0.3.0**，破坏性裁剪）：作者把富文本/源码
  编辑整块（`MarkdownEditor`、`editing.h`、source ranges 等）移入
  `archive/full-rich-text` 分支，库收敛为解析 + 只读渲染。apitab 只用
  `ParseMarkdown` + `Markdown` 只读渲染，公共 API 不变，无需改码；0.3 起也不再
  依赖 `TextField::TextStyles`。
- Lib-WebView fork `e3429b8` → `09c820a`：**刻意 pin 在删前状态**——fork tip
  `c0391ac` 随官方 SDK 0.4.0 迁移（作者 Scave `1deb796`）删除了 Linux WebKitGTK
  后端，而官方 SDK 已无 PlatformView 工厂、本仓跟的 HuxerUI fork 仍保留该工厂；
  fork 侧恢复后端后再跟进 tip。
- CodeEditor fork `870a82f` → `a332e7c`（本地 dev 与 CI 统一改用 fork）：
  剪贴板可靠性修复、worker 预解析文档接入、simdutf/库图目标等构建修复。
  **全部 7 个 sweetedit 本地补丁已删除**（FetchContent 兜底、滚动桥接、只读
  复制修复、ViewNode 重命名、菜单 label 悬空修复等均已收编进 fork）；保留
  嵌套 SweetEditor/SweetLine 的 oslog/locale 两个防御性补丁（嵌套依赖未动，
  fork `3dparty/patches/` 留有副本）。
- 嵌套 SweetEditor（`bd4330e`）/SweetLine（`64665d9`）上游无新提交，gitlink
  与 fork 声明一致。
