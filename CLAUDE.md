# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

apitab 是一个 **C++23 模块化 GUI 工具**：API 测试（libcurl 单请求）+ k6 压测 +
SQLite 存储，EUI-NEO 前端，用 **mcpp** 构建。套壳结构对齐旁边的 tinynext
（UI / 领域 store / 引擎三层分离，引擎自保留在 store 单例中）。

## EUI 参考顺序

UI 工作先阅读：

1. `docs/skills/eui-neo-ui-replicator/SKILL.md`（固定官方 snapshot，版本/commit 见 `UPSTREAM.toml`）
2. 已安装 EUI-NEO `0.5.7` 的 `site/llms.txt`、DSL、组件和 workshop 文档
3. `docs/eui-neo-compat.md` 与全局 `eui-development` overlay
4. 本文件和目标页面/store 模块

官方 skill 的 CMake、`apps/` 目录和广义 `eui_neo.h` 示例不适用于本项目；保持下列 mcpp/module 约定。

## 构建 / 运行

```bash
mcpp build          # 编译（dev）
mcpp build --release
./run.sh            # 启动 GUI（不要用 mcpp run：mcpp 私有 glibc 与系统 Mesa 冲突，
                    # run.sh 走系统 ld.so + 系统 Mesa，与 tinynext 同一个 launcher）
```

- 工具链固定 `llvm@22.1.8`，依赖版本都在 `mcpp.toml`：eui-neo **0.5.7**（features
  `app-main`）、curl **8.21.0**、sqlitecpp **3.3.3**、nlohmann::json **3.12.0**。
- **没有测试**：eui-neo 的 `app-main` 把 `glfw_app_main.o` 急切链入，与任何定义
  `main()` 的测试 TU 冲突（`multiple definition of 'main'`），tests/ 为空。
- **k6 不在 mcpp 仓库**：CI 打包时下载对应平台二进制放 `engines/` 随包分发；
  运行时按 `<exe>/engines/k6 → <exe>/k6 → <repo>/engines/k6（开发形态）→ PATH`
  解析（`src/config.cppm::cfg::k6Binary()`）。`engines/k6` 已 gitignore。

## 架构

全模块化（`import std` + 各 `apitab.*` 模块），UI 只面向引擎抽象：

| 模块 | 文件 | 职责 |
|------|------|------|
| `apitab.api_engine` | `src/api_engine.cppm` | 抽象接口 `ApiEngine` / `LoadEngine` + `RequestSpec` / `ResponseView` / `LoadOptions` / `LoadSummary` |
| `apitab.curl_engine` | `src/curl_engine.cppm/.cpp` | 单请求引擎：工作线程跑 `curl_easy_perform`，结果槽 + `requestUiUpdate()` 唤醒；`CURLOPT_XFERINFOFUNCTION` 协作取消 |
| `apitab.k6_engine` | `src/k6_engine.cppm/.cpp` | 压测引擎：生成 k6 脚本（`handleSummary` 打印 `K6SUMMARY {json}` 行）→ spawn 子进程 → 监视线程拆 `\r`/`\n` 行入队；stop=SIGINT，3s 宽限后 SIGKILL |
| `apitab.db` | `src/db.cppm/.cpp` | SQLiteCpp：requests / history / load_tests 三表；KV 序列化为 JSON |
| `apitab.config` | `src/config.cppm` | 数据目录（~/.local/share/apitab）/ k6 二进制解析 |
| `apitab.utils` | `src/utils.cppm` | 纯 string/number 帮助函数 + percentEncode / appendQuery |
| `apitab.store.requests` | `src/store/requests.cppm` | 领域 store：`g_requests` 持有 curl 引擎 + Db；集合 CRUD / send / pollResult（落历史） |
| `apitab.store.loadtest` | `src/store/loadtest.cppm` | 领域 store：`g_loadtest` 持有 k6 引擎；start/stop/drainOutput/pollSummary（落压测记录） |
| `apitab.store.ui` | `src/store/ui.cppm` | 视图 store：草稿 / 页面 / 响应 / 压测视图 / 状态消息（无 eui） |
| `apitab.ui.*` | `src/ui/*.cppm` | theme / utils(布局常量) / widgets / sidebar / request_page / loadtest_page / history_page |
| `src/app.cpp` | 普通 TU | 薄入口：`app::dslAppConfig()` + `app::compose()` |

**入口**：`main()` 由 eui-neo 的 `app-main` 提供，任何 TU 都不能再定义 `main()`。

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
4. **scrollView 的 content 是 column 弹性容器**：里面每个逻辑行必须包一层
   `ui.stack(rowId).size(w, h).content(...)` 占位，行内元素在 stack 里绝对定位；
   直接 `.position()` 的子项会被 column 重排（每个元素占一个竖排槽位）。
5. **无 position 的组件**（segmented / dropdown / dataTable）用
   `ui.stack("id.wrap").position(...).size(...)` 包裹定位，组件 `.size()` 填满。
6. **主色是白（深主题）/黑（浅主题）**：primary 按钮和 segmented 选中块文字必须
   反色 —— 按钮加 `.textColor/.iconColor(onPrimaryColor(theme))`，segmented 加
   `.style(segmentedStyle(theme))`（两个 helper 都在 `apitab.ui.widgets`）。
7. **缩放**：`DslAppConfig::uiScale(kUI)`（kUI=1.4）原生放大逻辑坐标系，尺寸按
   设计逻辑像素书写；窗口物理尺寸 = 设计尺寸 × kUI。
8. **k6 指标**：Trend 汇总默认只带 avg/min/med/max/p(90)/p(95)——脚本 options 里
   已声明 `summaryTrendStats` 加 p(99)，p50 用 `med` 键（没有 `p(50)`）。
9. **eui 元素 id 全局唯一**：同 frame 同名 id 会互相覆盖。
10. `curl_easy_setopt` 多线程必须 `CURLOPT_NOSIGNAL=1`（已设）。
