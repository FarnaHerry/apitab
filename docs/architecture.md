# apitab 架构与运行时资源模型

本文记录 apitab 的分层、线程契约与**运行时资源的所有权**（谁创建、谁释放、跨线程
怎么用），并给出"基础请求（curl）是否需要池化"的结论与依据。功能模块、引擎协议与日志约定也统一在本文维护。

## 1. 分层与依赖方向

```
UI 层        src/ui/*.cpp（普通 C++ + HuxerUI codegen）、src/app.cpp、platform/<platform>/main.cpp
              ↓ 只经 store 暴露的接口，不直接碰引擎指针
领域 store   apitab.store.requests / apitab.store.loadtest（自保留引擎与 DB）
              ↓ 只经 api::ApiEngine / api::LoadEngine 抽象
引擎层       apitab.api_engine（抽象）← apitab.curl_engine / apitab.k6_engine
基础设施     apitab.db / apitab.config / apitab.preferences / apitab.utils
```

- 依赖方向单向向下；引擎不 import store/UI。唯一反向上行是 `core::platform::
  requestUiUpdate()` 这个 UI 唤醒钩子（HuxerUI 的 State 失效模型下是 no-op，
  见 `src/app.cpp`），经全局模块片段前向声明，避免模块名修饰符号。
- 换引擎实现只动 `src/store/requests.cppm`（`makeCurlEngine()` 一处）与新增模块：
  UI 与 store 都只认 `api::ApiEngine`。
- curl 头只进 `src/curl_engine.cpp`（`curl_engine.cppm` 只导出工厂），第三方头不
  扩散到其它 TU。

## 2. 线程

| 线程 | 数量 / 归属 | 职责 | 生命周期 |
|------|-------------|------|----------|
| 主线程（UI 线程） | 1 | HuxerUI 组合、State 读写、协程轮询 | 进程 |
| HuxerUI worker 执行器 | Runtime 管理 | `RunWorker` 的阻塞 / CPU 工作（TCP 会话、解析等） | HuxerUI Runtime |
| CurlEngine 工作线程 | 1，引擎持有 | 串行消费请求队列、执行 curl 传输 | 引擎构造 → 析构 join |
| K6 监视线程 | 每次压测 1 条 | 读 k6 子进程输出、EOF 后收尾 | `start()` → 进程结束 / 析构 join |
| IXWebSocket 内部线程 | 每 WS 会话 | 握手 / 收发 / 回调 | `WsSession` 构造 → 析构 stop+join |

纪律：**State 只在 UI 线程读写**；UI 事件处理器可以同步更新 State，纯状态更新不需要
`Delay(0)`；composable 组合阶段不得写 State，挂载后同步使用 `Lifecycle`。引擎结果经
`PollWhile` 按节拍取回；阻塞活经 `co_await huxerui::RunWorker(...)` 交给 HuxerUI
Runtime 执行，恢复点在 UI 线程；worker 不访问 State/View。框架外线程通过
`TaskScope::Post` 投递 UI 工作。`src/ui/task_bridge.h` 只封装协程轮询工具。

## 3. 资源所有权矩阵

| 资源 | 拥有者 | 创建 | 释放 | 跨线程规则 |
|------|--------|------|------|-----------|
| curl 全局状态 | 引用计数 `CurlGlobal`（`curl_engine.cpp`） | 首个 `EasyHandle` | 最后一个句柄析构 → `curl_global_cleanup` | 初始化必须先于任何句柄创建（非线程安全） |
| easy 句柄 | `CurlEngine::easy_`（**常驻 1 个**） | 引擎构造 | 引擎析构（join 工作线程之后） | 只由工作线程使用；构造在 UI 线程、使用在工作线程，不并发 |
| 连接缓存 / DNS 缓存 / TLS 会话缓存 | 挂在 easy 句柄内部 | 传输时 | 随句柄销毁 | 只能靠**句柄复用**保留；`curl_easy_reset` 不清这些 |
| 请求头表 / MIME 体 | 每次请求的栈上 RAII（`HeaderList` / `MimeHandle`） | `run()` | `run()` 返回时 | curl 不复制内容：必须活到 `curl_easy_perform` 返回 |
| 结果槽 / 进度槽 | `CurlEngine` | 引擎构造 | 引擎析构 | `resultMutex_` 保护；UI 轮询读、工作线程写 |
| SQLite 连接 | `RequestStore::db_`（`unique_ptr<db::Db>`） | store 构造 | store 析构 | 仅实例进程持有；CLI 客户端通过控制面访问，不打开数据库 |
| k6 子进程 | `K6Engine::child_`（`ChildProcess`，RAII） | `spawn()` | 监视线程 `reap()`；析构兜底"强杀 + 回收" | 终止信号可从 UI 线程发（`stop()`），句柄是原子的；`reap()` 在监视线程 |
| k6 输出管道 | `K6Engine::readPipe_`（`ChildPipe`，RAII） | `spawn()` | 监视线程 `close()`；析构兜底关闭 | 父进程只持读端；子进程写端在 `spawn()` 内随局部 guard 关闭（不关则读端永远等不到 EOF） |
| k6 临时脚本 | `K6Engine::scriptPath_` | `start()` | 监视线程结束前 `remove`；启动/线程失败路径同步删除 | 文件名带 pid，避免多实例互踩 |
| WS 会话 | 页面协程 `State<shared_ptr<WsSession>>` | 页面 | 页面卸载 / 重连替换 | IX 回调只投递事件队列，UI 泵 drain |
| TCP 会话 | 页面协程 `State<shared_ptr<TcpSession>>` | 页面 | 同上 | 同步 asio，只准在任务线程调用；`close()` 任意线程幂等 |
| 单实例锁 | 平台入口 `SingleInstance` | `main()` | 进程退出析构 | fd/HANDLE 由 RAII 释放 |
| 响应下载临时文件 | `TemporaryFileGuard`（`request_editor.cpp`，协程帧内） | 保存响应前 | 作用域退出（含协程被取消）同步 `File::Delete()` | 全程 UI 线程；析构不抛出 |
| 头像裁剪临时 PNG | `AvatarCropOutput`（`settings_avatar_crop.cpp`） | worker 线程跑 `magick` 后 | 结果销毁时 `remove`（成功路径先 `rename` 走） | `shared_ptr` 跨 `RunWorker` 回 UI 线程 |
| `popen` 流 / 注册表键 | `cfg::PopenPipe` / `cfg::RegKey`（`config.cppm`） | `systemPrefersDark()` | 作用域退出析构 | 仅启动时调用一次 |
| 应用日志文件 | `apitab::log`（`src/log.cpp`，进程级单例） | 首次写入（惰性建目录） | 进程退出（追加写，无需显式关闭） | mutex 串行；**日志不进 SQLite**（不与领域写抢 WAL 唯一写者），见本文「日志与领域数据」 |
| 控制面服务（loopback + 端点文件） | `apitab::control::ControlServer`（应用安装期 Provide） | `AppOptions::application_hooks` | 应用关闭析构（停线程 + 删端点文件） | IO 线程收发；命令经 `ApplicationPoster`（`TaskScope::Post`）回应用线程执行 |
| WebView（响应 Preview 标签） | 组合期持有的 `WebViewController` + 框架 PlatformView 宿主 | 预览档挂载（`ResponsePreviewPane`） | 预览档卸载（框架按 PlatformView 生命周期回收） | 仅 UI 线程；`LoadHtml` 经 `Lifecycle` 在挂载/新响应时重放 |

阻塞与 CPU 工作由 HuxerUI Runtime 的 worker 执行器调度；`apitab` 不再拥有自建任务线程池，
因此该执行器不属于应用侧 RAII 资源。

## 4. 单次请求（curl）要不要池化？

**结论：不做 N 路句柄池，做"常驻单句柄 + 每请求复位"。** 依据：

1. **并发度决定上限**。`ApiEngine::send` 是"替换"语义（busy 时取消旧请求），
   队列由**一个**工作线程串行消费，任一时刻至多一个在途传输。N 路句柄池没有
   任何并行收益，多出来的句柄只会空转。
2. **连接缓存是句柄私有的**。libcurl 的 keep-alive 连接、DNS 缓存、TLS 会话都
   挂在 easy 句柄内部，不是进程级资源。开 N 个句柄 = 把缓存切成 N 份私有副本，
   同主机请求命中既有连接的几率反而下降。串行场景下 **1 个句柄 = 最大复用率**。
3. **收益是实打实的**。旧实现每请求 `curl_easy_init` + `curl_easy_cleanup`，
   同主机第二次请求也要重做 TCP + TLS 握手（HTTPS 上是 2 RTT 起步 + 握手 CPU）。
   常驻句柄后第二次请求直接走缓存连接：`tests/test_curl_engine.cpp` 用回环服务器
   断言"两次直连请求 = 服务端只 accept 一次"。
4. **k6 不经过本引擎**。压测是独立进程、自带连接池与 VU 模型，引擎不需要为吞吐
   做任何优化；把池化和压测解耦，单次请求这条路径保持简单。

复用的正确性靠两件事保证，缺一不可：

- **每个请求前 `curl_easy_reset()`**：清掉上一请求的全部选项。上一请求可能是
  SSE（传输中把 `CURLOPT_TIMEOUT` 关成 0）、可能配了代理、可能带过 POSTFIELDS；
  复位保证这些不会漏进下一请求。curl 契约保证 reset **不动** live connections /
  DNS 缓存 / TLS 会话缓存 —— 正是我们要留的部分。
- **代理按请求显式下发**：`CURLOPT_PROXY` 即使为空串也要设置（空串同时关掉
  libcurl 的环境变量代理探测），这样"`RequestSpec::proxy` 为空 = 直连"是确定性
  契约，不被运行环境的 `http_proxy` 推翻；非空时清空 `CURLOPT_NOPROXY`，让用户
  显式配置的代理不被 `no_proxy` 环境变量旁路。

**什么时候该升级成池 / multi**：当 `ApiEngine` 出现真正的并发 send（例如多标签页
同时发送、或切换到 `curl_multi` 事件驱动）时。届时的扩展点是句柄池（N = 并发上限）
配合 `CURLSH` 共享 DNS 与 TLS 会话缓存，而不是简单地多开几个 easy 句柄。当前代码
已经把这层抽象收在 `EasyHandle` / `CurlGlobal` 两个 RAII 类型里，替换成本低。

## 5. RAII 约定

**资源一律 RAII 包裹（无例外）**：任何"获取 → 使用 → 释放"的东西都必须由一个
类型持有、在析构里释放。**禁止在函数末尾手写 cleanup，禁止把裸句柄
（`int fd` / `HANDLE` / `pid_t` / `FILE*` / 裸指针）存成成员或跨语句传递。**
本仓有常驻工作线程、常驻监视线程和会被取消的 UI 协程：一次 `std::bad_alloc`、
一次提前 `return`、一次取消就足以让泄漏累积到进程结束。既有包装与完整清单见本文 §3（所有权矩阵）：

- **C 句柄**：curl（`CurlGlobal` / `EasyHandle` / `HeaderList` / `MimeHandle`）、
  子进程与管道（k6：`ChildProcess` / `ChildPipe` / `SpawnFileActions`）、
  `popen` 输出流与 Win32 注册表键（config.cppm：`PopenPipe` / `RegKey`）、
  单实例锁（`SingleInstance` 持有 lock fd / mutex HANDLE）、
  SQLite（SQLiteCpp 的 `Database` / `Statement`，本身就是 RAII）。
  CurlGlobal 的引用计数保证全局 cleanup 晚于 easy cleanup；SpawnFileActions
  必须跨过可能抛 bad_alloc 的 argv 装配过程，不能先释放 posix_spawn 动作表；
- **C++ 资源**：对象生命周期交 `unique_ptr` / `shared_ptr`；pimpl
  （`WsSession` / `TcpSession` / `Db`）在析构里 stop / close；
- **线程"谁起谁 join"**：应用自己起线程的构造函数要么把 worker 起齐，要么自己置停止位
  并 join 已起的线程后再把异常抛出去——凑不齐时若让异常逸出，未完成构造的
  对象不会走析构，而 `vector<std::thread>` 析构碰上 joinable 线程会直接
  `std::terminate`。阻塞/CPU 工作使用 HuxerUI 的 `RunWorker`；worker 生命周期由
  框架 Runtime 管理，应用不自建或保存线程池；
- **临时文件/目录**用局部守卫：`settings_avatar_crop.cpp` 的
  `AvatarCropOutput`、`request_editor.cpp` 的 `TemporaryFileGuard`。协程里尤其
  要紧——页面卸载/TaskScope 取消会在 `co_await` 悬挂点销毁协程帧，末尾那行
  `DeleteAsync()` 根本不会执行，同步清理必须挂在守卫析构上（清理要能同步完成，
  如 `huxerui::File::Delete()`），并保证析构不抛出；
- **多步装配**（如 k6 spawn：建管道 → 建子进程 → 移交成员）统一用"局部 guard
  持有 + 成功后 `adopt()` / `release()` 移交成员"，失败路径与中间抛异常都由
  guard 收尾；
- **析构兜底要彻底**：子进程代理类析构 = 强杀 + 回收（不留僵尸进程）；
  延迟初始化的句柄析构 = 若已创建就释放。

`runSafely` 的异常兜底只保证线程存活和填充结果槽，不能代替资源释放。
新增或替换资源时，同步更新本文 §3 的资源所有权矩阵。

### 5.1 审计结论（2026-09-26）

按"每个获取点是否由类型持有"全量核查了 `src/`、`platform/`、`tests/` 的
`fd` / `HANDLE` / `pid` / `FILE*` / 线程 / 临时文件 / 第三方句柄，结果：

- 所有资源都已由 RAII 类型或 `unique_ptr` / `shared_ptr` 持有，**没有裸句柄成员，
  也没有在函数末尾手写 cleanup 的代码**（唯一"手写"的是 `SingleInstance` 自己的
  析构体 —— 那个类本身就是包装类型）。
- 本轮修掉的四处"包装不彻底"：
  1. `k6_engine.cpp` 的 `childProcess_` / `readPipe_` / `childPid_` / `readFd_`
     原本是裸成员、由监视线程手写收尾，且 Windows 分支在宽字符转换失败时漏掉
     刚建的两根管道句柄；现改由 `ChildProcess` / `ChildPipe` 持有；
  2. `K6Engine::start()` 里 `std::thread` 构造抛 `system_error` 时，子进程会活着
     且无人回收；现已在该分支强杀 + 回收 + 清临时脚本；
  3. `config.cppm::systemPrefersDark()` 原本手写 `pclose` / `RegCloseKey`；现由
     `PopenPipe` / `RegKey` 持有；
  4. `TaskPool` 当时已通过构造失败时 join 已启动 worker 满足 RAII；本轮进一步移除
     应用自建线程池并改用 HuxerUI `RunWorker`。`request_editor.cpp` 的下载临时文件
     在协程取消时不会执行末尾的 `DeleteAsync()`，现由协程帧内 `TemporaryFileGuard`
     同步清理。
- 未改动的已知良性项：`LoopbackHttpServer`（`tests/test_curl_engine.cpp`）的
  `listenFd_` 是裸成员，但该对象自身是 RAII 包装（析构 `stop()` 关 fd + join），
  所有失败路径都在对象存活期内，无泄漏。

## 6. 已知缺口

- **实例启动即建重资源**：平台入口在进入 GUI Runtime 前显式调用 store 的 `Open()`，
  打开 SQLite 并创建 curl 工作线程。`--cli` 客户端只转发命令，不构造这些资源。
- 每请求仍会新建一份 `curl_slist` 与（FormData 时）MIME 树；相对传输开销可忽略，
  不做缓存池。

## 7. 验证

| 测试 | 覆盖 |
|------|------|
| `test_smoke` | URL / query 解析等纯函数 |
| `test_curl_engine` | 常驻句柄连接复用、代理按请求生效、选项复位不泄漏（回环 HTTP 服务器统计连接/请求数；只在 POSIX 执行，Windows 跳过） |
| `huxerui_selection_matrix` | 构建选择矩阵（源码 / SDK / 强制离线通道） |

```bash
cmake -B build -G Ninja && cmake --build build --target apitab test_curl_engine --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
```

## 8. 模块职责与引擎协议

- `apitab.api_engine`（`src/api_engine.cppm`）提供 `ApiEngine`、`LoadEngine`、
  `WebSocketEngine`、`TcpEngine`，以及请求、响应、压测配置和汇总类型。
- `apitab.curl_engine`（`src/curl_engine.cppm/.cpp`）提供 `makeCurlEngine()`；
  curl 头只进入实现单元。工作线程串行执行，send 纯入队，以代际丢弃旧请求；
  cancel 协作打断、清理排队请求，取消后不投递结果。run 全程 try/catch 兜底填错误结果。
  传输期头和正文累积到 `takeProgress` 增量快照；识别 `text/event-stream` 后关闭总超时，
  长连接仍可取消。常驻一个 easy 句柄，每请求 reset；不做 N 路池。代理每次显式下发，
  空串为直连并关闭环境变量代理探测。句柄、头表和 MIME 的 RAII 见 §3/§5。
- `apitab.k6_engine`（`src/k6_engine.cppm/.cpp`）生成脚本、启动子进程，监视线程按
  `\r`/`\n` 拆行入队。POSIX stop 先 SIGINT，3 秒宽限后 SIGKILL；Windows 优先
  k6 REST API `PATCH stopped:true` 保留 summary，API 不可达再 TerminateProcess。
  spawn 按 `spec.proxy` 注入或剥除代理变量，空为直连；进程与管道由 RAII 持有。
- `apitab.db`（`src/db.cppm/.cpp`）以 SQLiteCpp 管理 requests/history/load_tests
  等领域表，KV 序列化为 JSON；配置、偏好与纯函数分别归 `apitab.config`、
  `apitab.preferences`、`apitab.utils`（`src/config.cppm`、`preferences.cppm`、`utils.cppm`）。
- `apitab.store.requests`（`src/store/requests.cppm`）持有 curl 引擎与 Db，负责组织、
  项目、集合 CRUD；`finalizeSpec` 替换 `{{var}}`、拼接 URL、合并全局 Cookie，
  `sendViaEngine`/`takeResponse`/`recordHistory` 负责发送、取回与落历史。
- `apitab.store.loadtest`（`src/store/loadtest.cppm`）持有 k6 引擎，负责
  start/stop/drainOutput/pollSummary 和压测记录持久化。
- UI 普通 TU 位于 `src/ui/*.cpp`；`src/app.cpp` 只声明 `Application{AppRoot, AppOptions}`
  并保留 no-op `requestUiUpdate`。平台 `main()` 先加载会话偏好、打开 store，再
  `RunApplication()`。`huxerui_add_app` 建目标并启用 hcg，领域模块以 FILE_SET 加入同一目标。

HTTP 的 UI 协程每拍先 drain `takeProgress`，再取完成结果；SSE/分块正文实时进入流式
视图，完成后交给只读编辑器。WS/TCP 会话由页面协程持有，IX 管理其内部线程，TCP 的同步
asio 操作用 `RunWorker` 执行。k6 的 drainOutput/pollSummary 经 `PollWhile` 在 UI 线程
写 State。worker 只捕获普通值，不读写 State、不操作 View；外部回调经 `TaskScope::Post`。
原生关闭/托盘回调里会销毁或隐藏窗口的操作，应按 `app_dialogs.cpp` 延后到回调返回后。
`requestUiUpdate` 仅保留旧引用，不新增调用。

## 9. k6 汇总与配置扩展

**k6 指标与深度配置**：Trend 汇总默认只带 avg/min/med/max/p(90)/p(95)——脚本
options 里已声明 `summaryTrendStats` 加 p(99)，p50 用 `med` 键（没有 `p(50)`）。
K6SUMMARY 协议（`k6_engine.cpp` 的 handleSummary 模板与 `ParseSummaryLine`
互为两端）扩展约定：checks 用 -1 哨兵表示"脚本没写"（C++ 侧必须按键存在性
判断，不能信默认值 0）；thresholdOk -1/0/1；expectedOkCount 为
`http_req_duration{expected_response:true}` 计数（-1 = 未提供）。**k6 ≥v2
实测（v2.3）**：`metrics[key].thresholds` 是 `{ 表达式: {ok} }` 对象而非旧版
数组（数组遍历会抛 `object is not iterable`，模板两种形态都收）；status 子
指标（`http_reqs{status:...}`）在汇总数据里已不存在，"非预期响应"由
requests - expectedOk 推导（expectedOk 无 count 时 UI 自动隐藏）。压测模式
（Constant/Stages/Iterations）、阈值（p95/失败率）、TLS 跳过与连接复用在
`LoadOptions`，UI 组装与模板生成共用 `AssembleOptions`（loadtest_page.cpp）
—— 加参数只改这一处 + LoadOptions，别再复制解析。

## 10. 日志与领域数据

**日志一律落文件，不进 SQLite**：诊断/审计信息（控制面命令、失败原因、启动事件）
写 `<dataDir>/logs/apitab-YYYYMMDD.log`（`src/log.h`，追加 + 每行 flush + 当日 8 MiB
上限 + 0600）。理由：日志是高频追加的运维信息，塞进库会与领域读写抢 WAL 的**唯一
写者**、把库文件撑大，而它对应用查询毫无价值。**领域数据才进库**（组织/项目/请求/
环境/历史/压测记录/自动化用例）；排查问题时先看日志文件，不要在库里加 `log` 表。
日志实现本身不得抛异常：写文件失败降级 stderr，绝不反过来影响调用方。
