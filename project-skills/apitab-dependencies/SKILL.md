---
name: apitab-dependencies
description: "维护 apitab 的依赖、CMake、源码/SDK 选择、宿主工具和 SweetEdit 补丁；依赖升级或构建接入变更时使用。"
---

# apitab 依赖与构建接入

## 必守契约

- 以根 AGENTS.md 的上游跟踪与补丁规则为准；不要为旧名称新增兼容层或别名。
- 开发构建优先源码；顺序为显式 HUXERUI_HOME 源码、仓库源码、已安装 SDK、Linux 离线 SDK。
  缺源码或 Linux 开发包时可回落 SDK；仅 APITAB_HUXERUI_FORCE_SDK=ON 明确跳过源码。
- 先检查活动构建的实际来源、头文件与 CMake 目标，再使用 API；不要让 CLI 注入 SDK 覆盖源码。
- CLI 工程自省块保留在 project() 前；HUXERUI_LIBRARY_GRAPH_ONLY 不启语言、不构建依赖。
- 依赖 tarball 与 SHA256 在 third_party/；源码 clone 不入库，指引与补丁必须入库。
- 新 HuxerUI API 同时验证源码与发布 SDK 通道；失败须如实报告，不把头文件检查当完整链接。

## 按需读取

- 修改构建选择、更新 HuxerUI、重建 hcg/hrc、查 Linux 环境或依赖版本：
  [源码与工具](references/source-and-tools.md)。更新后重建宿主工具、清理旧生成缓存，
  按 docs/plans/ 的版本检索规则检查能力；先检查旧补丁是否能删。
- 修改 SweetEditor 文本装载、主题、键盘/滚动/选择器适配，或更新嵌套依赖：
  [SweetEdit 接入与补丁](references/sweetedit.md)。本地 clone 与 CI 重放须保持一致。
- 修改应用入口、codegen 文件布局或资源集成：
  [HuxerUI 接入说明](../../docs/huxerui-migration.md)。
- 修改运行库、CI 编译器、发布/安装器：
  [发布技能](../apitab-release/SKILL.md)，不要按通用 SDK 打包示例覆盖本项目格式。

## 验证

使用根指引的 Ninja 配置和 apitab 完整编译链接流程；按改动运行依赖选择矩阵、
引擎与相关 UI 测试。强制 SDK 验证用独立构建目录，避免破坏当前开发构建。
