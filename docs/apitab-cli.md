# apitab CLI（agent 可用）

apitab 的 CLI 子命令模式：`apitab --cli <子命令> [参数]`。它是**运行中实例的薄客户端**：
命令发给实例、在它的应用线程上执行，读写的正是你眼前那份状态（当前组织/项目/环境、
打开的项目标签），所以 agent 看到的就是用户看到的。

## 使用前提

- 二进制：CMake 直构产物 `./build/apitab`（本项目默认形态）；HuxerUI CLI 流程产物
  在 `.huxerui/build/linux/`（命名相同）。
- **需要有一个运行中的 apitab 实例**（阶段 0 起，见
  `docs/plans/runtime-control-surface.md`）：`--cli` 是运行中实例的**薄客户端**，
  命令在实例的应用线程上执行，结果经本机 loopback 控制面回传。
  实例未运行时打印错误并以 `1` 退出；`--cli --ensure <命令>` 可先拉起实例。
- 客户端进程**不进事件循环，也不打开数据库**：`RequestStore`/`LoadStore` 的打开是显式的
  （GUI 进程在进事件循环前 `Open()`），客户端只做控制面转发。实测客户端用假 HOME 跑
  `help`/`orgs`/`projects` **不产生任何文件**，所以跨进程读写同一个库的 BUSY 问题消失。
- `help`（含子命令 `--help`）在本地打印，**不需要实例**；其余命令需要实例。
- `--ensure` 拉起的实例会把 stdin/stdout/stderr 接到 `/dev/null`（Windows
  `DETACHED_PROCESS`），不会吊住调用者的管道（`out=$(apitab --cli --ensure …)` 可正常返回）。
- **控制面并发**：一条连接一个线程，`send`（最长 120s）只占住它自己的连接；期间其它
  命令（含 `ping`）照常返回。`send` 本身也拆成「应用线程准备 → 控制面线程等响应 →
  应用线程收尾」，所以整段传输期间 GUI 不冻结（实测 3s 端点上并发 `ping` 24ms）。
- 端点：`$XDG_RUNTIME_DIR/apitab-control.json`（0600，含端口与随机 token），
  随实例退出删除；控制面只绑 127.0.0.1 且拒绝无 token 请求。
- 命令在**有状态的实例**上执行：读的是它当前的组织/项目/环境与打开的项目标签。
  命令由 `SessionRestore` 快照/恢复上下文；临时切换与 active_project 写回在命令返回前还原。

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
| `send <请求ID> [--project ID] [--env ID\|名字] [--json]` | 组装→finalizeSpec（环境变量替换+baseUrl 与目录 Path 链拼接+合并全局 Cookie/公共头+全局超时/代理）→curl 引擎发送→10ms 轮询取回（120s 兜底），成功落历史并把响应 `Set-Cookie` 归集进项目 Cookie | `apitab --cli send 14 --project 5 --json` |
| `history [--limit N]` | 最近发送历史（默认 20 条，最新在前：ID/时间/方法/状态/耗时/大小/URL/错误/关联请求） | `apitab --cli history --limit 5` |
| `lightweight [off]` | **进程形态**操作（不进 `cli::run`）：进入/退出轻量模式——关闭窗口释放渲染资源与应用侧可再生缓存，退出时重建主窗口；控制面继续服务，其它子命令照常可用。幂等。托盘菜单也有对应入口 | `apitab --cli lightweight` / `apitab --cli lightweight off` |

### send 的要点

- `--env ID|名字`：发送前切换环境（数字=ID，`0`=无环境；否则按环境名精确匹配），
  供本次请求的 `finalizeSpec` 替换变量与拼接 baseUrl；收尾时按 SessionRestore 恢复规则处理。
- `--json`：单行结构化 JSON `{ok,method,url,status,durationMs,sizeBytes,headers,body,error}`
  ——脚本与 AI 解析首选；不含字段即无（如 headers 空数组、error 空串）。
- Mock 启用条目的语义与 GUI 一致：不走真实网络、不落历史、直接返回模拟响应。
- 非 HTTP 条目（WS/TCP）send 会明确报错退出 1。
- 发送超时强约束：引擎保证任何端点都在有限时间内返回（默认 30s 超时，全局设置可改），
  另加 120s 墙钟兜底后协作打断。唯一例外：响应头识别为 `text/event-stream`（SSE）
  时关闭总超时下放长连接，仍受取消/兜底打断约束。

## 项目/组织上下文规则

- 无参数时，命令针对运行中实例的上下文；内部 ensureContext 也会读取会话偏好。
- `--project ID`、`--org ID` 和 show/send 的跨项目定位只用于本次命令。
  SessionRestore 记录原组织、项目、环境和 active_project，在返回前恢复。
- send 的守卫跨准备/等待/收尾三段持有，必须在应用线程 FinishSend 中恢复一次；
  不依赖可能在控制面线程执行的 SendHandle 析构。
- 当前实现的边界：只恢复大于 0 的原组织/项目/环境 ID；原值为 0 时没有完整的反选恢复。
  不应把这一实现描述为客户端独立状态或承诺所有空上下文均恢复。

## AI 使用建议

- 批量校验场景：`requests` 拿 ID 列表 → `show` 逐个核对字段 → `send --json` 断言
  响应 `ok`/`status`/`body`。
- 别在循环里高频 `send`：每次都会落一条历史记录（与 GUI 发送同一条写入路径）。
- 修改类操作（增删改请求/组织/项目/设置）**不在** CLI 范畴，走 GUI 或 db 迁移；
  CLI 是只读 + send 的接口面。
- 输出若含中文引号/转义，`--json` 的 body 已按 JSON 转义，解析用 `jq` 等标准工具。

## 实现备注（改代码时参考）

- 平台入口识别 `--cli`：help 本地 `cli::run`，其它命令经 `src/control_client.cpp`
  转发到 `src/control.cpp`；命令实现在 `src/cli.cpp`，状态操作由实例应用线程执行。
- 控制面客户端有自己的传输超时（当前 `kForwardTimeoutMs = 90000`），
  与服务端 send 的 120s 墙钟上限是不同层级；修改超时时须一并核对。
- **目录即路由**：`finalizeSpec` 用 `RequestSpec.groupId`（CLI 从集合项的 `group_id` 带入）
  经 `composeUrl` 拼 `baseUrl + 目录 Path 链 + 路径`，Path 模式目录逐级累加
  （`api(P) > v1(P)` → `/api/v1`），Name 模式目录不贡献路由；输入带 URI scheme 时
  前缀全部失效、原样直发。URL 行只读前缀段（`urlPrefix`）与拼接共用同一套分段规则，
  所以界面上看到的前缀就是实际会发送的前缀。
- 只有实例显式 Open store 并持有 SQLite/curl 工作线程。send 准备与收尾在应用线程，
  控制面线程每 10ms 轮询响应，避免传输等待冻结 GUI；引擎结果槽支持跨线程取用。
- **响应 Cookie 归集**：`g_requests.collectResponseCookies(view)` 必须在每条发送路径
  的收尾调用一次（CLI 在 `cli.cpp` 取回结果处、GUI 在 `request_editor.cpp` 取回结果
  处，与 `recordHistory` 成对）。语义：同名（区分大小写）覆盖值、保留用户的启用/
  停用选择、首次出现写成启用、`Max-Age<=0` 或 `Expires` 已过期则删除同名项；
  只认第一段 `name=value`，属性段仅用于判过期。项目 Cookie 没有 domain/path 维度：
  任何响应返回的 Cookie 都进本项目列表，并随本项目之后每次发送一起带上（合并进
  单条 `Cookie:` 头，RFC 6265 §5.4，见 `src/curl_engine.cpp`）。
- 项目上下文/退出码/`--json` 的输出形状改动时，同步本专题文档及 [CLI 技能](../project-skills/apitab-cli/SKILL.md)。
