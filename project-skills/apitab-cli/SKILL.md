---
name: apitab-cli
description: "通过 apitab --cli 操作运行中实例，查询组织、项目、请求和历史，发送 HTTP 或切换轻量模式；批量校验及控制面开发时使用。"
---

# apitab CLI

操作前读取 [CLI 契约](../../docs/apitab-cli.md)，确认参数、输出、退出码及上下文规则。
控制面设计与后续工作见 [控制面计划](../../docs/plans/runtime-control-surface.md)。

## 核心语义

- `./build/apitab --cli <命令>` 是运行中实例的薄客户端；客户端不打开数据库、不建引擎。
- help 在本地执行，无需实例；其余命令需要实例，可用 `--cli --ensure <命令>` 先拉起。
- 实例应用线程执行状态操作；send 分为应用线程准备、控制面线程等待、应用线程收尾。
- 命令临时切换组织/项目/环境，以 SessionRestore 在返回前还原；具体边界以专题说明为准。
- stdout 放数据，stderr 放错误；退出码 0 为成功、1 为用法/数据/控制面错误、2 为传输失败。
  HTTP ≥400 的响应不等于传输失败；自动化优先解析 `send --json`。
- 真实发送会写历史并归集 Cookie；Mock 不走网络、不落历史。WS/TCP 不支持 CLI send。
- CLI 不提供领域 CRUD；变更请求、组织或设置使用现有应用入口或正式迁移流程。

## 按任务操作

- 批量校验：requests 获取 ID，show 核对配置，send --json 检查响应；注意网络及持久化副作用。
- 排障先看文件日志；端点与实例条件见 CLI 契约。
- 修改上下文、命令、退出码或 JSON 形状，同步专题文档；技能入口仅保留稳定流程。
- 修改发送路径，保证 recordHistory 与 collectResponseCookies 的收尾语义及会话恢复。
