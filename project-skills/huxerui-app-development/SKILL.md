---
name: huxerui-app-development
description: "开发 apitab 的 HuxerUI 页面、交互、主题、布局与应用侧组件；涉及 UI 声明、挂载或平台控件时使用，不用于框架内部开发。"
---

# HuxerUI App Development（apitab）

本技能从项目原有 HuxerUI 官方技能副本迁入，本轮未同步上游。
本地约定以根 AGENTS.md、项目专题文档及 apitab-runtime.md 为准。

## 先确认契约

- apitab 用 CMake；源码优先，不套用通用技能的“仅已安装 SDK”前提。
  构建选择、实际头文件与宿主工具见 [依赖技能](../apitab-dependencies/SKILL.md)。
- 只使用当前来源的公开 API、头文件与目标；不猜旧名称、不新增兼容别名。
- UI 用普通 .cpp，composable 不加 inline，View 按值传递。
- 先读 [DSL 风格](references/dsl-style.md) 与 [公共组件契约](references/public-contracts.md)。
- 状态、线程、RAII 按根指引；Theme 只提供环境，View 行为放内部内容容器。

## 项目约定（修改相关部分前必读）

- 页面结构、尺寸、滚动、PageCard 与分隔线：[UI 布局](../../docs/ui-layout.md)。
- 菜单、弹窗环境、方法配色、Hover、托盘和 WebView：
  [apitab 运行时与交互](references/apitab-runtime.md)。
- 编辑器文本重载、palette、只读复制与补丁：
  [SweetEdit](../apitab-dependencies/references/sweetedit.md)。
- 应用入口和 codegen 接入：[工程说明](../../docs/huxerui-migration.md)。

## 按需读取 API 分册

以下来自原官方技能副本；示例是参考，精确 API 以活动源码/SDK 公开头为准。

| 任务 | 引用 |
|---|---|
| State、组合、事件、生命周期、异步任务 | [fundamentals](references/fundamentals.md) |
| 控件、Select/ComboBox、日期时间与受控值 | [components](references/components.md) |
| 布局、滚动、虚拟化、响应式、坐标转换 | [layout](references/layout-and-ui.md) |
| With、Key、修饰器所有权 | [modifiers](references/modifiers.md) |
| 指针、Hover、菜单事件、拖放 | [gestures](references/gestures-and-drag-drop.md) |
| 内置机制不足时的保留扩展 | [extensions](references/node-extensions.md) |
| 主题、颜色、动画、菜单、弹窗、过渡 | [theme/presentation](references/theme-animation-presentation.md) |
| 输入、选择、语义与无障碍 | [text/semantics](references/text-input-and-semantics.md) |
| 资源、locale、剪贴板、文件、HTTP | [resources](references/resources-files-network.md) |
| 导航、窗口、安全区、托盘 | [navigation](references/navigation-and-window.md) |
| 系统通知 | [notifications](references/local-notifications.md) |
| Canvas、画笔、外部纹理与 GPU 帧 | [paint](references/canvas-paint-and-images.md) |
| 非可视平台服务 | [platform modules](references/platform-modules.md) |
| 原生嵌入控件 | [platform views](references/platform-views.md) |
| 公共 API 与头文件索引 | [API index](references/api-index.md) |
| 通用 CLI 工程模板（须服从 apitab 构建约定） | [workflow](references/project-workflow.md) |
| 阅读历史 mcpp 工程，非 apitab 当前构建流程 | [modules](references/cpp-modules-and-mcpp.md)、[mcpp](references/mcpp-build.md) |

## 更新与验证

- 更新官方副本时合并维护这些本地补充：gestures 的 Hover、components 的 Select、
  theme-animation-presentation 的 Presentation services，以及 apitab-runtime.md。
  不直接整目录覆盖本项目入口；记录来源，检查所有相对链接，再运行指引检查。
- 修改后验证受影响的受控状态、Key、键鼠与焦点、尺寸/滚动边界、深浅主题和挂载行为。
- Theme 边界变动覆盖 `tests/test_ui_layout.cpp::OpenSettingsTheme`；PlatformView
  合成变动覆盖 `tests/test_preview_native.cpp` 的原生 Preview 回归。
- 按根指引完成应用编译和链接；实际跑过的平台和通道才能报告验证通过。
