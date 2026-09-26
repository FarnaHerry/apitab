# apitab 架构与运行时资源模型

本文记录 apitab 的分层、线程契约与**运行时资源的所有权**（谁创建、谁释放、跨线程
怎么用），并给出"基础请求（curl）是否需要池化"的结论与依据。功能模块清单见
`CLAUDE.md` 的「架构」一节；本文只回答"东西归谁管、什么时候没的"。

## 1. 分层与依赖方向

```
UI 层        src/ui/*.cpp（普通 C++ + HuxerUI codegen）、src/app_main.cpp
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
| CurlEngine 工作线程 | 1，引擎持有 | 串行消费请求队列、执行 curl 传输 | 引擎构造 → 析构 join |
| 任务线程池 TaskPool | `clamp(hw,2,8)`，进程级静态 | `RunOnTaskThread` 的阻塞 / CPU 重活（TCP 会话、DB 重活） | 首次使用 → 进程退出 join |
| K6 监视线程 | 每次压测 1 条 | 读 k6 子进程输出、EOF 后收尾 | `start()` → 进程结束 / 析构 join |
| IXWebSocket 内部线程 | 每 WS 会话 | 握手 / 收发 / 回调 | `WsSession` 构造 → 析构 stop+join |

纪律（详见 `src/ui/task_bridge.h`）：**State 只在 UI 线程读写**；引擎结果经
`PollWhile` 按节拍取回；阻塞活经 `co_await RunOnTaskThread(...)` 上任务线程，恢复
点恒为 UI 线程；事件处理器内不同步写会卸载点击节点的 State。

## 3. 资源所有权矩阵

| 资源 | 拥有者 | 创建 | 释放 | 跨线程规则 |
|------|--------|------|------|-----------|
| curl 全局状态 | 引用计数 `CurlGlobal`（`curl_engine.cpp`） | 首个 `EasyHandle` | 最后一个句柄析构 → `curl_global_cleanup` | 初始化必须先于任何句柄创建（非线程安全） |
| easy 句柄 | `CurlEngine::easy_`（**常驻 1 个**） | 引擎构造 | 引擎析构（join 工作线程之后） | 只由工作线程使用；构造在 UI 线程、使用在工作线程，不并发 |
| 连接缓存 / DNS 缓存 / TLS 会话缓存 | 挂在 easy 句柄内部 | 传输时 | 随句柄销毁 | 只能靠**句柄复用**保留；`curl_easy_reset` 不清这些 |
| 请求头表 / MIME 体 | 每次请求的栈上 RAII（`HeaderList` / `MimeHandle`） | `run()` | `run()` 返回时 | curl 不复制内容：必须活到 `curl_easy_perform` 返回 |
| 结果槽 / 进度槽 | `CurlEngine` | 引擎构造 | 引擎析构 | `resultMutex_` 保护；UI 轮询读、工作线程写 |
| SQLite 连接 | `RequestStore::db_`（`unique_ptr<db::Db>`） | store 构造 | store 析构 | 进程内单连接；GUI/CLI 两进程靠 SQLite 文件锁串行 |
| k6 子进程 | `K6Engine::child_`（`ChildProcess`，RAII） | `spawn()` | 监视线程 `reap()`；析构兜底"强杀 + 回收" | 终止信号可从 UI 线程发（`stop()`），句柄是原子的；`reap()` 在监视线程 |
| k6 输出管道 | `K6Engine::readPipe_`（`ChildPipe`，RAII） | `spawn()` | 监视线程 `close()`；析构兜底关闭 | 父进程只持读端；子进程写端在 `spawn()` 内随局部 guard 关闭（不关则读端永远等不到 EOF） |
| k6 临时脚本 | `K6Engine::scriptPath_` | `start()` | 监视线程结束前 `remove`；启动/线程失败路径同步删除 | 文件名带 pid，避免多实例互踩 |
| WS 会话 | 页面协程 `State<shared_ptr<WsSession>>` | 页面 | 页面卸载 / 重连替换 | IX 回调只投递事件队列，UI 泵 drain |
| TCP 会话 | 页面协程 `State<shared_ptr<TcpSession>>` | 页面 | 同上 | 同步 asio，只准在任务线程调用；`close()` 任意线程幂等 |
| 单实例锁 | 平台入口 `SingleInstance` | `main()` | 进程退出析构 | fd/HANDLE 由 RAII 释放 |
| 任务线程池 | `detail::TaskPool` 进程级静态 | 首次 `RunOnTaskThread` | 进程退出静态析构（stop + join） | worker 起不齐时构造内先 stop + join 已起的线程，再把异常抛出去 |
| 响应下载临时文件 | `TemporaryFileGuard`（`request_editor.cpp`，协程帧内） | 保存响应前 | 作用域退出（含协程被取消）同步 `File::Delete()` | 全程 UI 线程；析构不抛出 |
| 头像裁剪临时 PNG | `AvatarCropOutput`（`settings_avatar_crop.cpp`） | 任务线程跑 `magick` 后 | 结果销毁时 `remove`（成功路径先 `rename` 走） | `shared_ptr` 跨 `RunOnTaskThread` 回 UI 线程 |
| `popen` 流 / 注册表键 | `cfg::PopenPipe` / `cfg::RegKey`（`config.cppm`） | `systemPrefersDark()` | 作用域退出析构 | 仅启动时调用一次 |

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

**任何 C 句柄都必须有 RAII 包装，禁止在函数末尾手写 cleanup。** 理由是异常路径：
`run()` 里任何 `std::bad_alloc`（构造头表、拼 URL、拷贝正文都可能抛）都会跳过
函数尾部的释放语句，而工作线程是常驻的 —— 泄漏会累积到进程结束。

现有包装与本仓自研 RAII 清单：

- `curl_engine.cpp`：`CurlGlobal`（全局状态，引用计数决定 cleanup 时机，避免静态
  析构顺序把 `curl_global_cleanup` 提到 `curl_easy_cleanup` 之前）、`EasyHandle`、
  `HeaderList`、`MimeHandle`；
- `k6_engine.cpp`：`ChildProcess`（子进程句柄，析构兜底"强杀 + 回收"不留僵尸）、
  `ChildPipe`（管道端，`adopt()` / `release()` 移交）、`SpawnFileActions`
  （`posix_spawn` 文件动作表，生命周期跨会抛 `bad_alloc` 的 argv 构造）；
- `config.cppm`：`PopenPipe`（`popen` 流，析构 `pclose`）、`RegKey`（Win32 注册表键，
  析构 `RegCloseKey`）；
- `RequestStore`：`std::unique_ptr<db::Db>` / `std::unique_ptr<api::ApiEngine>`；
- `WsSession` / `TcpSession` / `Db`：pimpl `unique_ptr` + 析构里 stop/close；
- 线程：一律"谁起谁 join"（`CurlEngine`、`K6Engine`、`TaskPool` 静态析构）；
  `TaskPool` 构造函数在 worker 起不齐时先 stop + join 再把异常抛出去——否则
  未完成构造不会走析构，而 `vector<std::thread>` 析构碰上 joinable 线程会
  `std::terminate`；
- 临时文件：`AvatarCropOutput`（`settings_avatar_crop.cpp`）、`TemporaryFileGuard`
  （`request_editor.cpp`，协程帧内，覆盖"保存对话框开着就切页/关标签"的取消路径）；
- `SingleInstance`：fd / HANDLE 由析构释放。

`run()` 的异常兜底（`runSafely`）只解决"线程不能死、结果槽必须填"，**不能**代替
资源释放。

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
  4. `TaskPool` 构造函数与 `request_editor.cpp` 的下载临时文件（协程被取消时
     末尾的 `DeleteAsync()` 根本不会执行）；分别改为"起不齐就自己 join 再抛"与
     协程帧内的 `TemporaryFileGuard`。
- 未改动的已知良性项：`LoopbackHttpServer`（`tests/test_curl_engine.cpp`）的
  `listenFd_` 是裸成员，但该对象自身是 RAII 包装（析构 `stop()` 关 fd + join），
  所有失败路径都在对象存活期内，无泄漏。

## 6. 已知缺口

- **k6 不消费 `RequestSpec::proxy`**：全局代理目前只对 curl 引擎生效。k6 只认
  `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY` 环境变量，要让它跟随应用设置需要给
  `posix_spawn` / `CreateProcessW` 显式传一份注入过的环境表（当前子进程继承父进程
  environ）。属于待办，不是"已生效但没接线"。
- **进程启动即建重资源**：GUI 与 CLI 都在 `RequestStore` 构造时打开 SQLite 并起
  curl 工作线程；`apitab --cli orgs` 这类不发送的只读命令也会带着它们。收益只有
  一次线程创建，暂不做懒启动。
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
