# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

apitab 是一个 **C++23 模块化 GUI 工具**：API 测试（libcurl 单请求）+ k6 压测 +
SQLite 存储，用 **CMake** 构建。前端为 **HuxerUI**（组件式声明 UI，官方预编译
SDK 接入）；迁移地图见 `docs/huxerui-migration.md`。分层：UI（src/ui/*.cpp +
app_main.cpp，普通 C++ 源走 hcg codegen）/ 领域 store（apitab.* 模块）/ 引擎
三层，引擎自保留在 store 单例中。

## HuxerUI 开发参考

UI 工作先读官方 skill：`.claude/skills/huxerui-app-development/SKILL.md`
（从 HuxerUI 官方仓库同步；references/ 含 dsl-style、components、fundamentals、
layout-and-ui、theme-animation-presentation、navigation-and-window 等分册）。
要点：

- SDK truth：`find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared)`，
  `HuxerUI_DIR` 指向 third_party 解包产物（`build/vendor/huxerui/...`）。
- UI 层是**普通 .cpp**（不要 .cppm：codegen 只扫 .cpp/.cc/.cxx）；composable
  函数不加 `inline`；入口/根写在 src/app_main.cpp + src/ui/app.cpp。
- 主题从 MaterialTheme/MaterialDarkTheme 等内置主题定制；受控值以应用状态为
  权威；动态兄弟用稳定 Key。
- 框架资源：resources.bin 拷到 `<exe>.resources/huxerui/`（POST_BUILD 完成）。

## 构建 / 运行

```bash
cmake -B build -G Ninja            # 配置（默认 Release；调试加 -DCMAKE_BUILD_TYPE=Debug）
cmake --build build -j             # 编译
ctest --test-dir build             # 冒烟测试（test_smoke）
./run.sh                           # 启动 GUI（切到仓库根 + INTEL_FORCE_PROBE=1）
```

- 工具链：系统 GCC（本机 16.2.1）+ libstdc++，CMake ≥ 4.4（`import std` 仍是
  experimental：版本相关 UUID 表在 `cmake/CxxImportStdGate.cmake`）。
- **依赖全部 vendor 在 `third_party/`**（tarball + SHA256，configure 期解包到
  `build/vendor/`，离线可复现；版本与原 mcpp.lock 一致，清单见
  `third_party/README.md`）：HuxerUI 0.1.0 SDK（官方预编译，shared 组件）、
  Asio 1.38.1（`import asio` 模块在 `cmake/asio.cppm`）、IXWebSocket 12.0.1
  （client-only、无 TLS/zlib）、curl 8.21.0（OpenSSL 后端）、SQLiteCpp 3.3.3
  （内置 amalgamation）、nlohmann::json 3.12.0（`import nlohmann.json` 模块在
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
| `apitab.preferences` | `src/preferences.cppm` | 会话偏好（settings.ini 的 session.*），跨会话状态恢复 |
| `apitab.ui.*`（普通 C++） | `src/ui/*.cpp` | HuxerUI 前端：app（导航壳）/ home_page / request_page / common |
| `src/app_main.cpp` | 普通 TU | 入口：`Application{AppRoot, AppOptions}` + `RunApplication()`；`requestUiUpdate` no-op 钩子 |

**入口**：`huxerui_add_app(apitab SOURCES ...)` 生成 app 目标并启用 hcg codegen；
领域层模块经 FILE_SET 追加到同一目标，`main()` 在 `src/app_main.cpp`。

**事件驱动**：HuxerUI 是 State 驱动失效模型。引擎异步结果由页面内
`UseTaskScope().Launch` 协程轮询 `pollResult/pollSummary/drainOutput` 并直接写
State（UI 线程调度）；旧 `requestUiUpdate` 唤醒钩子保留为 no-op。

## 关键约定（改代码前必读）

1. **`import std;` 后禁止再 `#include` 标准头**。C/系统/第三方头放全局模块片段
   （`module;` 与 `module apitab.x;` 之间）。
2. **跨模块前向声明（如 `core::platform::requestUiUpdate`）必须放全局模块片段**——
   写在模块域内会被附加模块名修饰（`@apitab.xxx`），链接不到外部符号。
3. **UI 层遵守官方 skill 的 DSL 风格**（`.claude/skills/huxerui-app-development/
   references/dsl-style.md`）：普通 .cpp、composable 不加 inline、View 按值传递。
4. **asio 一律经 `import asio;`**（`cmake/asio.cppm`，SSL 由 `APITAB_ASIO_SSL` 门控）。
   不要在全局片段 `#include` asio 头：模块 BMI 与头文件各自的内部链接静态对象
   （prefer/require/query 的 static_instance）在 GCC 下会重复定义。
5. **受控值以应用状态为权威**（官方 skill：controlled values）；TextField 保留
   完整 TextEditingValue；动态兄弟用稳定 `.Key(...)`。
6. **异步结果**：页面 `UseTaskScope().Launch` 协程轮询引擎并写 State；
   `requestUiUpdate` 是 no-op 钩子（app_main.cpp），不要新加调用。
   **事件处理器内禁止同步写会导致点击节点被卸载的 State**（如切页/关标签/
   切主题）——pointer-up 处理中同步重组会卸载按钮子树，框架随后 erase
   PointerSession 段错误。必须经 `tasks.Launch` + `co_await Delay(0)` 推迟；
   组合体内也不要写 State（挂载路径重入），初始值在 UseState 之前算好。
7. **k6 指标**：Trend 汇总默认只带 avg/min/med/max/p(90)/p(95)——脚本 options 里
   已声明 `summaryTrendStats` 加 p(99)，p50 用 `med` 键（没有 `p(50)`）。
8. **curl_easy_setopt 多线程必须 `CURLOPT_NOSIGNAL=1`**（已设）。

## CMake 迁移备注（原 mcpp 行为对照）

- Windows：HuxerUI 运行时 dll 在 POST_BUILD 拷到 exe 旁；OpenSSL 走 chocolatey，
  CI 里以 `OPENSSL_ROOT_DIR` 注入。
- CI 的 linux job 用 ubuntu:26.04 容器 + clang-21/libc++（详见
  `.github/workflows/build.yml` 注释）。- 版本号唯一来源是顶层 `project(apitab VERSION ...)`；packaging 脚本与 NSIS 从
  CMakeLists 解析。
