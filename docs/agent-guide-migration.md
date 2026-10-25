# Agent 指引迁移清单

本次迁移将原 487 行根指引拆成简短入口、项目技能与按需引用。
下表的原位置是迁移前 AGENTS.md 的行号，用于核对必要约束的去向；不是第二份规则来源。
项目技能统一保存在 project-skills/；CLAUDE.md 仍只引用根 AGENTS.md。

| 原位置 / 内容 | 详细维护位置 |
|---|---|
| 1–9 项目简介与分层 | [根指引](../AGENTS.md)、[架构](architecture.md) |
| 12–22 官方技能与三处本地补充 | [UI 技能](../project-skills/huxerui-app-development/SKILL.md)及保留的分册 |
| 23–28 跟随上游、兼容层与补丁淘汰 | 根指引「依赖原则」 |
| 29–64 源码/SDK 选择、fork、宿主工具 | [源码与工具](../project-skills/apitab-dependencies/references/source-and-tools.md) |
| 65–123 SweetEdit 接入与七类补丁 | [SweetEdit](../project-skills/apitab-dependencies/references/sweetedit.md) |
| 124–156 UI 文件、Theme、WebView、自适应菜单 | [UI 运行时](../project-skills/huxerui-app-development/references/apitab-runtime.md) |
| 157–160 版本检索计划规则 | 根指引、源码与工具的更新流程 |
| 161–198 PageCard、单线分隔、色板、状态栏 | [布局专题](ui-layout.md) |
| 199–203 能力检测与双通道验证 | 源码与工具「能力检测与双通道」 |
| 204–278 托盘、右键、画笔、Hover、弹窗、方法色与危险操作 | UI 运行时各小节 |
| 279–306 构建、链接门禁、Git 交付、工作区保护 | 根指引「构建与验证」「工作区与交付」 |
| 307–311 发布入口 | [发布技能](../project-skills/apitab-release/SKILL.md) |
| 312–340 工程格式、库图扫描、工具链、vendor、k6 发现 | 源码与工具「工程与工具链」 |
| 341–376 模块表、引擎行为、线程与 UI 桥 | 架构 §1–4、§8 |
| 379–391 模块头文件、前向声明、DSL、asio、受控值 | 根指引「关键约定」1–5、UI 技能 |
| 392–402 State、协程、worker、原生回调 | 根指引第 6 条、架构 §2/§8 |
| 403–415 k6 指标、哨兵、thresholds、AssembleOptions | 架构 §9 |
| 416–419 Spacer 与有界根链 | 根指引第 8 条、布局专题 |
| 420–449 RAII、构造失败、取消、多步装配、资源矩阵 | 架构 §3/§5，合并原有重复内容 |
| 450–456 文件日志、大小、权限与失败降级 | 根指引第 10 条、架构 §10 |
| 457–478 CI、标准库、运行库、WiX/MSIX | [CI 与平台](../project-skills/apitab-release/references/ci-platforms.md)、[打包契约](../project-skills/apitab-release/references/packaging.md) |
| 479–487 CLI 薄客户端与控制面 | [CLI 技能](../project-skills/apitab-cli/SKILL.md)、[CLI 专题](apitab-cli.md) |

现有 HuxerUI references 原样保留其 API 分册与三处本地补充；公共应用约束另收在
[public-contracts](../project-skills/huxerui-app-development/references/public-contracts.md)。
通用 SDK workflow 增加本项目来源选择/打包约定的优先说明；历史 mcpp 分册去掉无法在本仓库
解析的上游相对链接，仍保留其历史用途，不将 mcpp 构建步骤作为 apitab 当前流程。

同时纠正了旧文档的两类漂移：源码优先与通用“仅已安装 SDK”的冲突，以及 CLI 被描述为
独立开库/独立引擎的旧模型。CLI 上下文恢复边界和客户端超时依据现有实现记录，未修改行为。
宿主工具构建参数依据当前源码改为 HUXERUI_HOST_TOOLS_STATIC_RUNTIME=OFF。

维护时先迁移/合并再删原文，引用统一指向一个详细来源。检查命令为
`python3 scripts/check_agent_docs.py`；需先将新增文件暂存，才能通过 Git 跟踪检查。
脚本核对体量、仓库内 Markdown 链接与资源跟踪状态；约束语义完整性仍按本表人工复核。
