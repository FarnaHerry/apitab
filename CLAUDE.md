# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

apitab 是一个 **C++23 模块化 GUI 工具**：API 测试（libcurl 单请求）+ k6 压测 +
SQLite 存储，用 **CMake** 构建。**本分支正在把前端从 EUI-NEO 迁移到 HuxerUI
（组件式声明 UI，SDK 接入）**——迁移地图与进度见 `docs/huxerui-migration.md`。
领域层（src/store/*、引擎、DB、config）与 UI 框架无关。

## EUI 参考顺序

UI 工作先阅读：

1. `docs/skills/eui-neo-ui-replicator/SKILL.md`（固定官方 snapshot，版本/commit 见 `UPSTREAM.toml`）
2. 已解包的 EUI-NEO `0.5.7` 的 `site/llms.txt`、DSL、组件和 workshop 文档
   （`build/vendor/eui_neo/eui-neo-0.5.7/site/`，即 third_party 解包产物）
3. `docs/eui-neo-compat.md` 与全局 `eui-development` overlay
4. 本文件和目标页面/store 模块

官方 skill 的 CMake、`apps/` 目录和广义 `eui_neo.h` 示例不适用于本项目；保持下列模块约定。

## 构建 / 运行

```bash
cmake -B build -G Ninja            # 配置（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译
ctest --test-dir build             # 冒烟测试（test_smoke）
./run.sh                           # 启动 GUI（切到仓库根 + INTEL_FORCE_PROBE=1）
```

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 3.30（`import std` 仍是
  experimental：版本相关 UUID 在 `cmake/CxxImportStdGate.cmake`，升级 CMake 后若
  configure 报 "incorrect value" 就去该 CMake 版本的
  `Help/dev/experimental.rst` 查新值）。
- **依赖全部 vendor 在 `third_party/`**（tarball + SHA256，configure 期解包到
  `build/vendor/`，离线可复现；版本与原 mcpp.lock 一致，清单见
  `third_party/README.md`）：eui-neo 0.5.7（bundled 3rd/，glfw+opengl，无
  tray/markdown，`glfw_app_main.cpp` 由 apitab 目标自己编译）、Asio 1.38.1
  （`import asio` 模块在 `cmake/asio.cppm`）、IXWebSocket 12.0.1（client-only、
  无 TLS/zlib）、curl 8.21.0（OpenSSL 后端）、SQLiteCpp 3.3.3（内置
  amalgamation）、nlohmann::json 3.12.0（`import nlohmann.json` 模块在
  `cmake/nlohmann.json.cppm`）。
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
| `apitab.curl_engine` | `src/curl_engine.cppm/.cpp` | 单请求引擎：工作线程跑 `curl_easy_perform`，结果槽 + `requestUiUpdate()` 唤醒；`CURLOPT_XFERINFOFUNCTION` 协作取消 |
| `apitab.k6_engine` | `src/k6_engine.cppm/.cpp` | 压测引擎：生成 k6 脚本（`handleSummary` 打印 `K6SUMMARY {json}` 行）→ spawn 子进程 → 监视线程拆 `\r`/`\n` 行入队；stop=SIGINT，3s 宽限后 SIGKILL |
| `apitab.websocket_engine` / `apitab.tcp_engine` | `src/*.cppm/.cpp` | IXWebSocket / Asio 实现，回调只投递事件 |
| `apitab.db` | `src/db.cppm/.cpp` | SQLiteCpp：requests / history / load_tests 三表；KV 序列化为 JSON |
| `apitab.config` | `src/config.cppm` | 数据目录（~/.local/share/apitab）/ k6 二进制解析 |
| `apitab.utils` | `src/utils.cppm` | 纯 string/number 帮助函数 + percentEncode / appendQuery |
| `apitab.store.requests` | `src/store/requests.cppm` | 领域 store：`g_requests` 持有 curl 引擎 + Db；组织/项目/集合 CRUD / send / pollResult（落历史） |
| `apitab.store.loadtest` | `src/store/loadtest.cppm` | 领域 store：`g_loadtest` 持有 k6 引擎；start/stop/drainOutput/pollSummary（落压测记录） |
| `apitab.store.ui` | `src/store/ui.cppm` | 视图 store：草稿 / 页面 / 响应 / 压测视图 / 状态消息（无 eui） |
| `apitab.ui.*` | `src/ui/*.cppm` | theme / utils(布局常量) / widgets / sidebar / topbars / request_page / tcp_page / websocket_page / loadtest_page / history_page / home_page / settings_page |
| `src/app.cpp` | 普通 TU | 薄入口：`app::dslAppConfig()` + `app::compose()`；`main()` 来自编入同目标的 eui `glfw_app_main.cpp` |

**入口**：`main()` 由 eui-neo 的 `core/app/glfw_app_main.cpp` 提供（CMake 下作为
apitab 目标的普通源文件编译，见顶层 `CMakeLists.txt`）；apitab 自己的 TU 不能再
定义 `main()`，tests/ 里的可执行文件是独立目标（`tests/test_smoke.cpp`），与
app-main 无冲突。

**渲染循环 / 事件驱动**：`app::compose` **不能挂 `.onFrame`**（eui 会当成「每帧都
在动」强制满帧重绘）。引擎完成时 `core::platform::requestUiUpdate()` 唤醒一帧，
compose 里 `pollResult/pollSummary/drainOutput` 取回结果写视图 store。空闲时 UI
走 `glfwWaitEvents` 睡眠，零渲染零 CPU。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统/第三方头放全局模块片段
   （`module;` 与 `module apitab.x;` 之间）。
2. **跨模块前向声明（如 `core::platform::requestUiUpdate`）必须放全局模块片段**——
   写在模块域内会被附加模块名修饰（`@apitab.xxx`），链接不到 eui 的符号。
3. **UI 模块加 include 用 `"eui_ui.h"`**（src/ui/ 下的精简头），不要用完整 `<eui_neo.h>`。
4. **asio 一律经 `import asio;`**（`cmake/asio.cppm`，SSL 由 `APITAB_ASIO_SSL` 门控）。
   不要在全局片段 `#include` asio 头：模块 BMI 与头文件各自的内部链接静态对象
   （prefer/require/query 的 static_instance）在 GCC 下会重复定义。
5. **scrollView 的 content 是 column 弹性容器**：里面每个逻辑行必须包一层
   `ui.stack(rowId).size(w, h).content(...)` 占位，行内元素在 stack 里绝对定位；
   直接 `.position()` 的子项会被 column 重排（每个元素占一个竖排槽位）。
6. **无 position 的组件**（segmented / dropdown / dataTable）用
   `ui.stack("id.wrap").position(...).size(...)` 包裹定位，组件 `.size()` 填满。
7. **主色是白（深主题）/黑（浅主题）**：primary 按钮和 segmented 选中块文字必须
   反色 —— 按钮加 `.textColor/.iconColor(onPrimaryColor(theme))`，segmented 加
   `.style(segmentedStyle(theme))`（两个 helper 都在 `apitab.ui.widgets`）。
8. **缩放**：`DslAppConfig::uiScale(kUI)`（kUI=1.4）原生放大逻辑坐标系，尺寸按
   设计逻辑像素书写；窗口物理尺寸 = 设计尺寸 × kUI。
9. **k6 指标**：Trend 汇总默认只带 avg/min/med/max/p(90)/p(95)——脚本 options 里
   已声明 `summaryTrendStats` 加 p(99)，p50 用 `med` 键（没有 `p(50)`）。
10. **eui 元素 id 全局唯一**：同 frame 同名 id 会互相覆盖。
11. **curl_easy_setopt 多线程必须 `CURLOPT_NOSIGNAL=1`**（已设）。

## CMake 迁移备注（原 mcpp 行为对照）

- eui 子项目形状 = 原 `compat.eui-neo`：`EUI_ENABLE_TRAY=OFF`（原构建是
  `EUI_TRAY_HAS_BACKEND=0` 桩）、`EUI_ENABLE_MARKDOWN=OFF`（md4c 原本就没编）、
  `EUI_DEPS_MODE=bundled`。
- GCC 16 对 eui vendored 的 `3rd/nanosvgrast.h` 有一处 `isnan` 兼容补丁
  （`third_party/CMakeLists.txt` 末尾，幂等）。
- Windows 上 eui 不带 `EUI_HAS_CURL`（其 network 桩化，apitab 自己的 curl 引擎
  不受影响）；OpenSSL 走 chocolatey 包，CI 里以 `OPENSSL_ROOT_DIR` 注入。
- 版本号唯一来源是顶层 `project(apitab VERSION ...)`；packaging 脚本与 NSIS 从
  CMakeLists 解析。
