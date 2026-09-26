# 计划：运行中实例的控制面（有状态自动化接口）+ 持久化层迁移到 HuxerUI::SQLite

状态：**待决策/未实施**。

## 0. 定位（这一版的前提）

apitab 是 **GUI 为主**：人在这里组织项目、环境、请求、压测。控制面的存在意义不是
"再做一个 HTTP CLI"（那块比不过 curl，agent 也不会用），而是让 agent 能操作
**用户正在用的这个实例**——同一个项目、同一个环境、同一批草稿与历史——并产出人能用
的东西（**接口文档**、批量导入、结果汇总）。

因此基线是"**有状态**"：

- 接口读的是**会话态**（当前组织/项目/环境、打开的项目标签、未保存草稿），
  不只是库里的死数据；
- 写操作走 store 的**同一条路径**，UI 立即反映，不存在两个进程各写一份库；
- 反面教材（本计划明确放弃）：`apitab --cli` 自己开 SQLite 的**无头 CLI**方向。
  它既打不过 curl，又把 CLI 变成唯一没有 Runtime 的 DB 使用者，逼出"headless 泵"这
  类没人想维护的东西。**方向错误，废弃。**

## 1. 三个值得做的 agent 工作流（控制面的验收标准）

1. **AI 写接口文档（旗舰）**：agent 读运行中项目的全部接口（方法/URL/参数/头/Cookie/
   body 示例/环境变量/目录链）→ 生成 Markdown（或 OpenAPI）→ 落到库里 →
   **请求页「文档」分区展示撰写版**（今天的文档页是只读、按草稿现算的，
   `src/ui/request_doc.cpp`，没有存储位）。
2. **AI 把外部接口描述落进实例**：把一份 OpenAPI/Swagger/Postman JSON 交给 agent，
   它直接落到当前项目的目录里（解析已有 `src/ui/api_import.cpp` 可复用）。
3. **AI 汇总运行结果**：读历史/压测记录 → 出报告；读的是"这个实例的上下文"，
   这正是有状态接口相对"一份孤立 SQL 库"的价值。

## 2. 控制面形态

```
agent / 脚本 / apitab --cli（薄客户端：只发请求、打印结果）
   │  读 $XDG_RUNTIME_DIR/apitab/control.json（0600：host/port/token）
   │  POST JSON 命令
   ▼
运行中的 apitab（GUI 进程 = 唯一 DB 属主，唯一会话属主）
   ├─ apitab.control：asio loopback HTTP 服务（IO 线程只收发）
   ├─ ApplicationHandle::DispatchToApplicationThread(...) 投回应用线程
   ├─ 应用线程执行：读会话/读 store/写 store → db（ORM，异步）
   └─ 结果 → JSON 响应
```

- **传输**：127.0.0.1 一次性端口 + 每次启动随机 token（token 文件 0600，请求带
  `Authorization`）。跨平台、`curl` 可手工调试，栈里已有 asio。
- **接口形状**：JSON 命令面（不是"SQL 查询接口"）。两个面：
  - **读面**（第一阶段交付）：会话态（当前组织/项目/环境、打开的标签）、项目树、
    单请求全字段、环境变量、历史、压测记录。
  - **受限写面**（第二阶段）：新建/更新请求、导入接口、写文档字段、切环境。
    写用户正在编辑的对象需要乐观锁（`updatedAt` 比对），第一版建议只开放
    "新建/导入 + 文档字段"这类低冲突写面。
- **`apitab --cli` 保留但重定位**：薄客户端便捷壳（打一发控制面请求、按人来读的
  排版打印）。命令面可以比现在窄；`docs/apitab-cli.md` 与 `apitab-cli` skill 的
  "不启动 GUI、不进事件循环"承诺要改成"操作运行中的实例"。
- **未运行实例时**：见 §5 决策（建议默认报错 + 可选 `--ensure` 拉起）。

## 3. Lib-SQLite 的约束（为什么它正好只适合 GUI 进程）

**执行必须活在 HuxerUI 运行时里**（类型化/编码部分是纯头文件模板，可离线）：

- `Database::OpenAsync` → `co_await state->operations.Run(...)`，`operations` 是
  `huxerui::WorkerSequence`（库 `src/sqlite.cpp:1504`）；`Task` 恢复走
  `detail::ResumeTask` → `TaskExecution::QueueResume`
  （`third_party/huxerui/src/runtime/task.cpp:562`），该队列由 Runtime 主循环泵。
- 库自带集成测试必须构造 `Application` + platform 桩 + `Runtime` 并手写 `RunNext()`
  泵循环（`tests/integration_tests.cpp:260` 起）。
- **无 Runtime 时 `co_await` 不报错、永不恢复**（静默挂死）。

控制面把 DB 全部收进 GUI 进程后，这条约束**只影响一个进程**，正是我们要的位置；
`--cli` 侧不再有 DB，也就不再有任何 `co_await`。

另：apitab 的 `SQLiteCpp` 与 lib-sqlite 各自内置一份 sqlite3 amalgamation（均无符号
前缀），同一可执行里会重复定义 → 迁移是**替换**，不能双轨；阶段 1 必须在同一提交里
摘掉 SQLiteCpp。

## 4. 阶段划分

**阶段 0 — 控制面地基（不碰 ORM，可独立交付）**
1. `apitab.control`（服务端：loopback + token 文件 + `DispatchToApplicationThread`
   回到应用线程）+ 薄客户端；先只做**读面**：会话态、项目树、单请求全字段、
   环境变量、历史。
2. CLI 重定位：`apitab --cli` 变成控制面薄客户端，输出契约（stdout 只放数据、
   错误走 stderr、退出码 0/1/2、`--json` 形状）逐字保持，做新旧对拍回归。
3. 文档/skill 重定位（`docs/apitab-cli.md`、`.claude/skills/apitab-cli`）。
4. 出成果：agent 已经能"读这个实例"→ 可以生成接口文档（文档落库留到阶段 3）。

**阶段 1 — 接库与 schema**
5. `third_party/huxerui-lib-sqlite`（git clone，跟 sweetedit 同款、gitignore）+
   `huxerui_use_library(apitab TARGET HuxerUI::SQLite PATH ...)`，同提交摘掉
   SQLiteCpp 与 `third_party/tarballs/SQLiteCpp-3.3.3.tar.gz`。
6. 9 张表现有结构定为 `Schema{1, ...}`；老库（本机现有 1 组织/7 项目/12 环境/
   5 请求/108 历史）走一次性 `Migration`（承接历史 `PRAGMA table_info` + `ALTER TABLE`
   的列，`src/db.cpp:257-294`）；迁移单测：老库副本 → 打开 → 断言。
7. 借迁移系统加**文档存储位**（如 `requests.doc`，见 §5 决策 3）。

**阶段 2 — `apitab.db` 异步化 + store/UI 改造**
8. `src/db.cppm` 接口改 `Task<Result<...>>`；store 同步 getter 改「异步加载 + 状态
   缓存」，写操作协程化；UI 组合体不再直接查库，改为读缓存 State + `Lifecycle`
   触发加载。**工作量主体，不是 SQL 改写本身。**

**阶段 3 — 写面与文档闭环**
9. 控制面写面：新建/更新请求、导入 OpenAPI/Postman、切环境、写文档字段（乐观锁）。
10. 请求页「文档」分区展示"撰写版（存库）+ 自动生成版"。
11. `ctest` + 三平台 CI + 更新 `CLAUDE.md`（依赖清单/模块表）与
    `docs/architecture.md`（资源矩阵 + 线程契约：DB 从"进程内单连接同步"改成
    "串行 worker 序列 + Task"）+ 发版。

## 5. 待决策

1. **实例未运行时**：a) 报错提示（退出码 1）；b) `--ensure` 显式拉起（托盘形态）再等
   端点；c) 默认自动拉起。建议 **a + b**。
2. **写面第一版范围**：只读 / 只读 + 文档字段 / 全量 CRUD。建议**只读 + 文档字段 +
   导入**（低冲突，先验证 agent 工作流）。
3. **撰写文档落点**：a) 新列 `requests.doc`（每请求一份，随请求走，迁移天然承载）；
   b) 项目级文档文件（Markdown 落在项目目录）；c) 只导出文件、不落库。建议 **a**
   （配合 b 作为导出目标）。

## 6. 风险

- **静默挂死**：阶段 0 完成后，`co_await` 应只存在于 GUI 进程；任何残留的无
  Runtime 异步调用都会挂住而不是报错。
- **UI 改造量**：阶段 2 是主要成本。
- **写面冲突**：agent 改动用户正在编辑的请求必须有乐观锁与明确报错，不能悄悄覆盖。
- **控制面安全边界**：只绑 127.0.0.1、token 0600 且每次启动重生成、拒无 token 请求。
- **定位纪律**：控制面是"操作运行中实例"，不要长出"离线批处理 CLI"这类分支——
  那条路已经判定为错误方向。

## 7. 轻量级模式（伪纯 CLI）可行性 —— 已实测

目标形态：进程留在托盘/后台，只跑 runtime 泵 + DB（ORM）+ 控制面 + 托盘图标，
agent 全程经控制面操作；人不在场也不需要窗口。

### 7.1 实测（本机 KDE/Wayland，本地构建，`window.Hide()` = 关到托盘路径）

| 状态 | VmRSS | RssAnon | RssFile | 瞬时 CPU |
|---|---|---|---|---|
| 窗口可见 | 224.7 MB | 85.2 MB | 136.4 MB | 出帧时按需 |
| 关到托盘（Hide） | 224.5 MB | 85.0 MB | 136.4 MB | **≈ 0.0%** |

- **CPU：隐藏后基本归零**（窗口不再出帧）——伪 CLI 形态成立的核心理由。
- **内存：Hide 只省 0.25 MB（0.1%），等于不省**。原因在代码里也对得上：
  Linux 的 `Hide` 就是 `gtk_widget_set_visible(FALSE)`（`platform/linux/linux_adapter.cpp:745`），
  GL surface 与渲染缓存（字形图集/纹理/布局缓存）仍由 runtime 持有；hui 也没有公开的
  "释放缓存/内存压力"入口（`include/huxerui/*.h` 里 grep 不到 Trim/Purge/MemoryPressure/Evict）。
- 对照：用户那个已跑 3h42m 的 `/opt/apitab/apitab` 实例 Rss 30 MB / Pss 2.9 MB
  （库页高度共享），说明长时间后台驻留的**真实下限**取决于 libs 共享，而不是 Hide。

### 7.2 hui 能提供什么、不能提供什么

| 能力 | 现状 |
|---|---|
| 运行中隐藏窗口 | ✅ `WindowHandle::Hide()`（apitab 已用于"最小化到托盘"） |
| 隐藏时停止出帧 | ✅ 隐式（实测 CPU ≈ 0） |
| **启动即无窗口/纯后台** | ❌ 不是公开能力：`WindowOptions` 没有 visible/background 开关，`RunApplication()` 由平台建窗 |
| **关窗后重建窗口** | ❌ 没有公开 `OpenWindow`；"window retirement" 只出现在 `Runtime::Retire()`（进程退出）语境；多窗口仍是计划项（`docs/plans/project-tab-tear-off-window.md`） |
| 释放渲染缓存/内存压力回调 | ❌ 无公开 API（建议反馈上游） |

→ 现在能做的形态是"**有窗启动 → 关到托盘（Hide）→ 控制面继续服务**"；
"启动就后台、点托盘再建窗"要等 hui 提供无窗口启动或窗口重建能力。

### 7.3 要真省内存，两条路

- **apitab 侧（现在就能做）**：进入轻量模式时释放我们自己持有的重资源——响应正文/
  流式快照、SweetEditor 文档与语法 provider 缓存、图片 `ImageAsset`（头像/裁剪预览）、
  历史/压测列表缓存、k6 输出队列；恢复（tray Activate / 控制面无状态请求）时惰性重建。
  可控、可测；当前进程 anon 85 MB 里 huxerui runtime 占大头，预计可省 10–40 MB。
- **hui 侧（要上游）**：`TrimCaches()` 之类的公开入口释放字形图集/纹理/布局缓存——
  这才是大头，但今天做不了，列入"建议反馈作者"。

### 7.4 结论与落点

轻量级模式**值得做**（CPU 归零 + agent 可用的伪 CLI 形态 + 可控的内存回收），
但不要把它当成"Hui 的隐藏功能顺带省内存"：**Hide 不省内存**，省内存要 apitab 主动
释放 + 上游补缓存回收入口。

落点：并入阶段 0——控制面落地后，加 `lightweight` 命令（或托盘菜单项）=
`window.Hide()` + 上述释放；并在该阶段量三态（可见 / 隐藏 / 轻量）RSS 与 CPU 作为回归基线。


### 7.5 轻量模式实测（2026-09-26，Release，KDE/Wayland，X11 后端便于观察窗口）

| 状态 | VmRSS | 窗口 | 瞬时 CPU | 备注 |
|---|---|---|---|---|
| 启动后可见 | 206.7 MB | 1 | 按需 | |
| `--cli lightweight`（隐藏 + 释放缓存） | 206.6 MB | 0 | **≈ 0.0%** | 释放语法高亮 provider 缓存 + k6 输出缓冲 |
| `--cli lightweight off`（恢复显示） | 214.9 MB | 1 | 按需 | 重新 realize GL surface + 期间命令加载了 store 缓存 |

**结论（诚实版）**：

- 机制成立且可用：窗口隐藏 → 停止出帧、CPU 归零；控制面继续服务——轻量模式下
  `--cli orgs / projects` 照常返回，`lightweight` 幂等（重复调用给"已处于/当前不在"）。
- **内存收益在空闲态约等于 0**（Δ ≈ 0.1 MB）。原因是应用侧唯一的大块静态缓存
  （SweetLine provider）只有在打开过带编辑器的请求页后才有内容；空闲态它是空的。
- 因此"轻量模式省内存"目前**不成立**，它省的是 CPU + 提供了伪 CLI 形态。真正的大头
  （字形图集/纹理/布局缓存）仍在上游，需要 `TrimCaches()` 之类的公开入口。

**按实测修正的下一步**：

- 释放集要打到"工作会话里真的占内存"的对象：SweetEditor 文档/行索引（打开过请求页时）、
  响应正文与流式快照、头像 `ImageAsset`、历史/压测列表 State。这些都在组合状态里，
  需要一条"轻量模式订阅"把它们一并丢掉（而不是现在只清静态缓存）。
- 基线口径也要改：现在测的是"刚启动就进轻量"，应补"打开若干请求后再进轻量"的对照。

## 8. "CLI 拉起 → 默认到托盘" 还差什么

> **TODO（等上游 HuxerUI）：`WindowOptions::start_hidden` / 无窗口启动。**
> 现状：Linux 适配器启动序列先 `gtk_window_present()` 再出首帧，应用代码只能在首帧
> 组合里 `Hide()`，所以"启动即进入托盘"必然闪一下窗口。本仓先把其余部分做完，
> 该能力等作者支持后接入（届时删掉"拉起时闪一下"的说明与 workaround 代码）。

目标形态（用户确认的方向）：`apitab --cli <cmd>` 发现实例没跑 → 把它拉起到**托盘 +
轻量模式** → 经控制面执行命令。这样进程里 runtime/DB(ORM)/curl 引擎/k6/托盘全在，
agent 的数据类命令（orgs/projects/requests/show/send/history）全部可用。

三个前提 + 一个代价：

**前提 A — 控制面本身（阶段 0，尚未存在）**
今天的 `apitab --cli` 是**自开库的无头进程**（`platform/*/main.cpp` 直接 `cli::run()`），
不是客户端。没有服务端就没有"拉起后可用"这回事。

**前提 B — 拉起与握手（新写）**
今天第二次启动走到单实例锁就 `return 0` **静默退出**（`platform/linux/main.cpp`）。
新逻辑：发现实例 → 连控制端点；没有端点 → 拉起 + 等端点就绪（超时兜底）→ 失败给明确
错误码。并发拉起天然被单实例锁串行化。

**前提 C — hui 没有"启动即隐藏"**
`WindowOptions` 无 visible/background 字段；Linux 适配器的启动序列是
`CreateWindow → InitializeWindow → running_=true → gtk_window_present() → RequestFrameAt()`
（`platform/linux/linux_adapter.cpp:640-654`），**先 present 再出首帧** → 应用代码只能在
首帧组合里 `window.Hide()`，因此**会闪一下窗口**（数十至数百 ms；apitap 现有
`hideToTray` 还多一跳 `Delay(0)`，启动路径没有回调栈约束，可直接 Hide）。
要彻底无闪，需上游提供 `WindowOptions::start_hidden`（或允许运行时建窗/无窗口启动）
—— 列入上游反馈项。

**代价/边界 — 隐藏时不出帧**
实测隐藏态 CPU ≈ 0，说明不再出帧；UI 状态失效要等到恢复显示才重绘。runtime 定时器
（`PollWhile` / `Delay`）按设计仍应触发，**但必须专门验证**：否则 agent 的写操作在 UI 上
"没反应"，直到用户点开窗口。数据类命令不受影响。

**验证清单（并入阶段 0）**
1. 拉起后控制面可用：`--ensure` → 端点就绪 → 命令返回，客户端退出码正确。
2. 隐藏态下 runtime 定时器/任务仍在跑（写一条请求后恢复窗口，UI 立即是新状态）。
3. 三态基线（可见 / 隐藏 / 轻量）RSS + CPU 记录成回归数据。
4. 启动闪烁时长实测（present → Hide 的间隔），作为是否催上游 `start_hidden` 的依据。

## 9. 阶段 0 进度

已完成（2026-09-26，提交 `feat(control): run --cli commands inside the running instance`）：

- [x] GUI 进程起控制面：loopback HTTP（一次性端口 + 随机 token）+ 端点文件
      `$XDG_RUNTIME_DIR/apitab-control.json`（0600，退出删除），只绑 127.0.0.1、
      拒无 token 请求（401）。
- [x] 命令在应用线程执行：`TaskScope::Post` 经 `ApplicationPoster` 注入（安装期还没有
      组合作用域，所以由根组合挂载时注入、卸载时清空）；60s 超时 → 504。
- [x] `cli::run(args, Sink&)` 单一实现：进程直跑写标准流，控制面收集成字符串回传——
      stdout/stderr 分流、退出码、`--json` 形状天然一致。
- [x] `apitab --cli` 变薄客户端：不构造 store、不碰数据库、不进事件循环；
      实例未运行时 stderr 提示 + 退出码 1。
- [x] 实测：`orgs` / `projects` / `requests` / `history --limit` 输出与旧实现一致；
      无实例路径错误可读。

已完成（第二轮）：

- [x] 只读子命令不回写会话：`SessionRestore` 包住整条命令，进入时快照 组织/项目/环境 +
      `active_project`，返回前（含异常路径）还原。命令期间的临时切换安全——整条命令在
      应用线程同步执行、不让出事件循环，组合观察不到中间态。
      实测：实例当前项目 11 → 跑 `projects --org 1`、`requests --project 5` → 再读仍是 11，
      settings.ini 的 `session.active_project` 逐字未变。
- [x] `--ensure` 拉起 + 握手：`apitab --cli --ensure …` 发现实例没跑就 `posix_spawn` /
      `CreateProcessW` 再跑一次自己（GUI 路径），轮询端点最多 30s；已在跑则直接复用
      （实测复用耗时 24ms）。默认仍是报错 + 退出码 1。

已完成（第三轮）：

- [x] **日志落文件、不进库**（用户定调，防与 SQLite 抢 WAL 唯一写者）：新增
      `src/log.h/.cpp`（`<dataDir>/logs/apitab-YYYYMMDD.log`，追加 + 每行 flush +
      当日 8 MiB 上限 + POSIX 0600 + 绝不抛异常），控制面生命周期与每条 agent 命令
      （参数/退出码/耗时/失败原因）都记进去；库里**不加**任何 log 表。
      实测：`--cli orgs` / 失败命令 / `lightweight` 均落行，SQLite 表清单无 log 表。

已完成（第四轮）：

- [x] **store 懒初始化**：`RequestStore` / `LoadStore` 的构造函数改为空壳，打开挪到显式
      `Open()`；GUI 入口在 `RunApplication()` 前 `g_requests.Open(); g_loadtest.Open();`，
      读库命令各自兜一次（`ensureContext` / `cmdOrgs` / `cmdHistory`）。
      实测：`--cli` 客户端用假 HOME 跑（help + orgs + projects）**建出 0 个文件**，
      实例侧一切照常。
- [x] `help` 离线可用：客户端对无参数/`help`/`--help`/`-h`（含子命令 `--help`）直接在
      本地打印，不需要实例、不碰数据库（实测：无实例 + 无 runtime dir → 打印帮助、exit 0）。
- [x] **就绪探测（ping）**：端点文件在监听成功后立刻写出，而命令投递口要等首次组合
      才挂上——中间那段窗口里发命令会拿到 503。现在 `InstanceRunning()` 用 `ping`
      走完整链路（含应用线程投递）作为判据，`--ensure` 冷启动 3/3 稳定通过。
- [x] **拉起的实例与调用者标准流解耦**：`posix_spawn` 把子进程 stdin/stdout/stderr 接到
      `/dev/null`（Windows 用 `DETACHED_PROCESS`）。否则 `out=$(apitab --cli --ensure …)`
      会一直等管道关闭（= 等实例退出），对 agent 就是"命令挂住"——本轮实测踩到过。

下一轮：
- [x] 轻量模式落地：控制面 `lightweight [off]` 命令 + 托盘菜单项「轻量模式（隐藏并释放
      缓存）」，隐藏窗口 + 释放应用侧可再生缓存；实测见 §7.5（CPU 归零成立，内存收益
      空闲态≈0，需按 §7.5 的修正下一步扩大释放集）。
- [ ] 轻量模式释放集扩到组合态对象（编辑器文档/响应正文/头像/列表 State）+
      "打开若干请求后再进轻量"的对照基线（§7.5）。
- [ ] `send` 走后端引擎异步化：现在它在应用线程上同步等到响应，会把 GUI 卡住整个传输时长
      （旧 CLI 是独立进程没这个问题）；保持 `--json` 形状不变。
- [ ] 启动即隐藏仍等上游 `WindowOptions::start_hidden`（§8 TODO）：拉起的实例现在会闪一下窗口。
- [ ] `help` 离线可用（不依赖实例），并让 CLI 进程彻底不构造 store（懒初始化）。
- [ ] 轻量模式（首帧 Hide + apitab 侧缓存释放）+ 三态 RSS/CPU 基线（§7）。
- [ ] `send` 走后端引擎（等最终结果 + 超时），保持 `--json` 形状。
