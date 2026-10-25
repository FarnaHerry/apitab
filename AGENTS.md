# apitab agent guide

本文件是仓库统一入口；`CLAUDE.md` 只引用本文件，不重复维护。
apitab 是 C++23 模块化 GUI 工具：curl API 测试、k6 压测、SQLite 存储，
CMake/Ninja 构建，HuxerUI 0.3.0 前端，支持源码与发布 SDK 两条通道。

## 指引维护规则

- 根 `AGENTS.md` 同时满足最多 150 行、6,000 字符；字符按 Unicode 计，包含换行。
- 每个项目级 `SKILL.md` 最多 100 行，包含 frontmatter、空行和代码块。
- 项目技能统一在 `project-skills/<技能名>/SKILL.md`，单独维护并纳入 Git。
- 长细节放该技能的 `references/` 或已有专题文档；项目级引用必须在本仓库并纳入 Git。
- 超限先合并重复内容或外移；禁止压成长行、删必要约束规避。每条约束只维护一个详细来源。
- 入口说明引用的适用任务，按需读取；不依赖个人目录、未跟踪 clone 中的指引副本。
- 修改指引后运行 `python3 scripts/check_agent_docs.py`，检查体量、链接及 Git 跟踪状态。

## 按任务读取

开始相关工作前读取对应入口，后续只加载涉及的小节：

| 任务 | 必读入口 |
|---|---|
| UI、交互、主题、布局、挂载 | [HuxerUI skill](project-skills/huxerui-app-development/SKILL.md) |
| 依赖更新、CMake、源码/SDK、宿主工具、编辑器补丁 | [依赖 skill](project-skills/apitab-dependencies/SKILL.md) |
| 工作流、packaging、发布产物生成或清理 | [发布 skill](project-skills/apitab-release/SKILL.md) |
| CLI 操作、批量校验、控制面开发 | [CLI skill](project-skills/apitab-cli/SKILL.md) |
| 模块、引擎、线程、资源、k6 协议、日志 | [架构](docs/architecture.md) |
| 页面表面、尺寸、分隔线、滚动与布局回归 | [UI 布局](docs/ui-layout.md) |
| HuxerUI 工程接入与应用编写约定 | [迁移说明](docs/huxerui-migration.md) |

## 架构边界

- UI：`src/ui/*.cpp`、`src/app.cpp`、`platform/<platform>/main.cpp`，普通 C++ 走 hcg。
- UI 经 store 接口操作；store 自保留 Db/引擎，只依赖 `api::ApiEngine` 等抽象。
  引擎不 import store/UI，curl 头只进入实现单元；模块职责与协议见架构文档。
- `src/app.cpp` 声明 Application；平台入口先加载会话偏好、打开 store，再运行应用。
- State、View 与引擎生命周期不能混用；异步结果回到 UI 线程后再更新界面。

## 关键约定（改代码前必读）

1. `import std;` 后禁止 include 标准头；C/系统/第三方头放全局模块片段。
   普通 UI `.cpp`/头文件必须显式 include 所用标准设施，不依赖传递包含。
2. 跨模块前向声明放全局模块片段，避免符号附加模块名而链接失败。
3. UI 用普通 `.cpp`，composable 不加 inline，View 按值传递；遵守技能的 DSL 风格。
4. asio 经 `import asio;`，SSL 由 `APITAB_ASIO_SSL` 门控；模块 TU 不在全局片段包含
   asio 头，避免 BMI/头文件内部链接对象重复定义。保留普通 TU 已有的 MSVC 头文件分支。
5. 受控值以应用状态为权威；TextField 保留完整 TextEditingValue；动态兄弟使用稳定 Key。
6. State 只在 UI 线程读写，组合期不赋值；初值先计算，挂载后同步用 Lifecycle。
   UI handler 直接同步改状态，不为纯状态更新包装 Launch + Delay(0)。引擎结果用
   PollWhile；阻塞/CPU 工作交 RunWorker，worker 只捕获普通值，不访问 State/View。
   外部线程用 TaskScope::Post；原生回调内的窗口销毁/隐藏按既有实现延后处理。
   `requestUiUpdate` 是 no-op，不新增调用。
7. k6 配置统一经 LoadOptions + AssembleOptions；扩展汇总协议前读架构文档的
   「k6 汇总与配置扩展」，同步脚本与解析端，不复制参数解析。
8. Spacer 自带 Grow(1)，零尺寸占位用空 Row/Column；页面根链逐层有界，内容在分区内滚动。
   表面、单线分隔、PageCard、菜单自适应等具体约束必须遵守 UI 布局与 UI 技能引用。
9. 资源一律由 RAII 类型持有；禁止函数末尾手写 cleanup、裸句柄成员或跨语句传递。
   线程谁起谁 join，构造失败也须回收已起线程；不自建应用级通用线程池。
   协程取消的临时资源须同步、无异常析构清理；多步获取先局部 guard，成功后移交。
   新增/替换资源同步更新架构文档的所有权矩阵，子进程析构须强杀并回收。
10. 日志写 `<dataDir>/logs/apitab-YYYYMMDD.log`，不进 SQLite；领域数据才入库。
    排障先看日志；每行 flush、每日 8 MiB、0600，失败降级 stderr，不抛异常影响调用方。

## 依赖原则

- HuxerUI、SweetEdit 及嵌套依赖跟随上游当前契约，不新增旧接口兼容层或本地别名。
- 本地补丁仅补上游未覆盖的功能/平台问题，保持最小、注明原因，并由 CI 重放。
  更新先检查能否删补丁，已上游化则同步删除补丁、CI 应用步骤及过时说明。
- 工程格式遵守 SDK 契约，开发构建源码优先；CLI 注入的已安装 SDK 不压过仓库源码。
  选择顺序、回落条件、宿主工具重建和双通道验证按依赖技能执行。
- HuxerUI 版本变化后按 `docs/plans/` 各计划的「版本检索规则」检查新版能力；未更新无需检索。

## 构建与验证

```bash
cmake -B build -G Ninja
cmake --build build --target apitab --parallel "$(nproc)"
ctest --test-dir build --output-on-failure
./run.sh
```

- 默认 Release；调试配置加 `-DCMAKE_BUILD_TYPE=Debug`。CLI 构建/运行路径见依赖技能。
- 完成代码、CMake、资源或生成输入修改后，立即执行对应完整构建，必须拿到 `apitab`
  编译和链接成功结果后才可宣布完成。仅 hcg、单个 .o、静态检查均不能代替。
- 不因依赖重建耗时跳过、后台遗留构建，或以“尚未完成构建”作为完成交付。
- 运行适用测试；UI Theme/PlatformView 边界变动须覆盖实际挂载与交互，不能只编译。
- 链接或测试失败，明确报告失败命令、直接原因及是否由本次改动引起；
  未链接对象不能作为运行验证结果。SDK 门禁与平台验证按对应技能执行。

## 工作区与交付

- 先检查 git status 与差异。保留既有无关修改，不重置、不覆盖、不混入提交。
- 完成任务并通过要求的验证后，仅暂存本任务文件，创建语义清晰的 commit，
  推送到当前分支上游远端。推送失败报告命令与直接原因，不反复重试或偏离任务。
- 指引迁移对应关系见 [迁移清单](docs/agent-guide-migration.md)；维护时保留必要约束的去向。
