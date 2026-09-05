---
name: apitab-cli
description: "Operate apitab from the command line (headless, no GUI): list orgs/projects/requests, inspect saved requests, send HTTP requests via the shared curl engine, and read send history. Use for `apitab --cli`, headless send/show automation, or when Claude (or a script) needs to drive apitab without the GUI."
---

# apitab CLI（agent 可用）

apitab 的 CLI 子命令模式：`apitab --cli <子命令> [参数]`。不启动 GUI、不进事件循环，
与 GUI **共用同一套数据**（`~/.local/share/apitab` 的 SQLite `apitab.db` 与 `settings.ini`、
同一个项目/组织/环境上下文），所以可用它做批量校验、状态读取与 headless 发送，
结果与 GUI 行为一致。

## 使用前提

- 二进制：CMake 直构产物 `./build/apitab`（本项目默认形态）；HuxerUI CLI 流程产物
  在 `.huxerui/build/linux/`（命名相同）。
- 运行时不依赖 GUI/显示；首次构造会打开/创建数据库（数据目录不可写会报错退出 1）。
- 与 GUI 同时运行安全：SQLite 按连接加锁，两进程读写靠锁串行，偶发 BUSY 已转成
  错误字符串返回（不崩溃）。

## 子命令速查

全部子命令支持 `--help` 查看单条详情；`help` 打印总览。语法约定：

- **stdout 只放数据**（列表一行一条 / show 块 / send 结果）；错误与提示一律 **stderr**。
- **退出码**：`0` 成功（含 HTTP 状态码 ≥400——状态在输出里，不算失败）；
  `1` 用法/数据错误（参数非法、ID 不存在、URL 为空等）；`2` 请求传输失败或超时。
- 列表命令用 `*` 标注“当前”项（当前组织/项目）。

| 子命令 | 作用 | 常用示例 |
|---|---|---|
| `help` | 总览 | `apitab --cli help` |
| `orgs` | 列出全部组织 | `apitab --cli orgs` |
| `projects [--org ID]` | 列出项目（默认当前组织） | `apitab --cli projects --org 1` |
| `requests [--org ID] [--project ID]` | 列出项目内请求（ID/方法/名称/URL/分组/更新时间；首行标注项目+环境上下文） | `apitab --cli requests --project 5` |
| `show <请求ID> [--project ID]` | 单请求全字段：params/headers/cookies/body（含表单字段）/测试用例/Mock 配置 | `apitab --cli show 14 --project 5` |
| `send <请求ID> [--project ID] [--env ID\|名字] [--json]` | 组装→finalizeSpec（环境变量替换+baseUrl 拼接+合并全局 Cookie/公共头+全局超时/代理）→curl 引擎发送→10ms 轮询取回（120s 兜底），成功落历史 | `apitab --cli send 14 --project 5 --json` |
| `history [--limit N]` | 最近发送历史（默认 20 条，最新在前：ID/时间/方法/状态/耗时/大小/URL/错误/关联请求） | `apitab --cli history --limit 5` |

### send 的要点

- `--env ID|名字`：发送前切换环境（数字=ID，`0`=无环境；否则按环境名精确匹配），
  只影响本次进程内的 `selectEnv`（finalizeSpec 的 `{{var}}` 替换与 baseUrl 拼接都读它），
  不持久化。
- `--json`：单行结构化 JSON `{ok,method,url,status,durationMs,sizeBytes,headers,body,error}`
  ——脚本与 AI 解析首选；不含字段即无（如 headers 空数组、error 空串）。
- Mock 启用条目的语义与 GUI 一致：不走真实网络、不落历史、直接返回模拟响应。
- 非 HTTP 条目（WS/TCP）send 会明确报错退出 1。
- 发送超时强约束：引擎保证任何端点都在有限时间内返回（默认 30s 超时，全局设置可改），
  另加 120s 墙钟兜底后协作打断。唯一例外：响应头识别为 `text/event-stream`（SSE）
  时关闭总超时下放长连接，仍受取消/兜底打断约束。

## 项目/组织上下文规则（与 GUI 一致）

- 什么都不给：从 `settings.ini` 的 `session.active_project` 恢复（GUI 启动时同样行为）。
- `--project ID`：跨组织反查所属 org 后切换，并**回写 active_project**（下次 GUI
  启动会打开到该项目标签——注意此副作用）。
- `--org ID`：切换组织；只做进程内内存切换，**不回写**偏好。
- `show/send` 内部跨项目自动定位目标请求。

## AI 使用建议

- 批量校验场景：`requests` 拿 ID 列表 → `show` 逐个核对字段 → `send --json` 断言
  响应 `ok`/`status`/`body`。
- 别在循环里高频 `send`：每次都会落一条历史记录（与 GUI 发送同一条写入路径）。
- 修改类操作（增删改请求/组织/项目/设置）**不在** CLI 范畴，走 GUI 或 db 迁移；
  CLI 是只读 + send 的接口面。
- 输出若含中文引号/转义，`--json` 的 body 已按 JSON 转义，解析用 `jq` 等标准工具。

## 实现备注（改代码时参考）

- `src/cli.cpp`（普通 C++ TU）+ `platform/*/main.cpp` 的 `--cli` 分支：
  `argv[1]=="--cli"` → `apitab::cli::run()`，GUI 路径零变化。
- 复用 `g_requests` 领域单例：构造即打开 SQLite 并持有 curl 引擎（常驻工作线程）。
  CLI 无事件循环，主线程 `sleep(10ms)` 轮询 `takeResponse`（结果槽内部加锁，跨线程
  取用安全；契约见 `src/curl_engine.cppm` 注释）。
- 项目上下文/退出码/`--json` 的输出形状改动时，同步本 skill 文档。