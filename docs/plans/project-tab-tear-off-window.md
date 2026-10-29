# 计划：项目标签拖出成独立窗口（tear-off）与拖回

状态：**初步实现，手动验证仍有问题，待修复**（2026-10-28）。
HuxerUI fork `farna/main` 的应用级多窗口 API 由 `ApplicationHandle::OpenWindow`
接收每个窗口自己的 `ViewFactory`；窗口位置以 `WindowOptions::initial_position` /
`WindowHandle::Position()` 暴露，X11/Windows 支持定位，Wayland 退化。2026-10-29 fork
`b083028` 移除了 `WindowHandle::Retire()`，统一使用会经过 `OnCloseRequest` 的 `Close()`；
apitab 用 `managed_closes_` 区分管理器关闭和用户原生关闭，避免拖回后重复 detach。
`quit_on_last_window_closed=false` 保持轻量模式无窗常驻。fork 的
`DragDropScope::Application` 支持跨窗口 typed drop；原生传输取决于宿主能力。

apitab 已接线 WindowManager 应用服务（窗口表/转移信封/主窗口桥）、二维拖拽拆出/拖回
手势、主页唤醒、✕ 关闭策略、焦点驱动领域游标重同步。用户手动验证反馈"仍有不少问题"，
具体问题清单待逐条记录后修复；此前验证过的部分：fork [linux-native] 原生多窗口测试
（开窗/退役/定位）、apitab 启动组合、构建链路。当前 fork 文档记录 Linux 浮动窗口完整输入
序列仍有待验证项，不能用独立开窗/拖拽测试代替该验证。

## 已知风险区（修复时优先排查）

1. 拆出/拖回瞬间子窗口组合（信封消费、领域游标时序）。
2. 双窗口领域游标串台（焦点重同步的覆盖面，后台轮询回写守卫尚未逐点审计）。
3. 子窗口 ✕ / 标签 ✕ 的关闭策略与主窗口 open_projects 持久化的一致性。
4. Wayland 下窗口落位退化（协议限制）与拖出武装态的视觉反馈。

## 需求

- 顶级项目标签（`ProjectTabStrip`）当前限定 X 轴拖拽换位
  （`DragGesture{.axis = Axis::Horizontal}`，app.cpp / request_page.cpp 均已锁定）。
- 目标交互：项目标签竖向（Y 轴）拖出标签条超过阈值时，把该项目的工作区
  拆成一个**独立 OS 窗口**；独立窗口里的标签可以**拖回**主窗口的项目标签条。
- 仅项目级标签生效；请求页内部标签（RequestTabStrip chips）不需要此功能，
  保持纯 X 轴换位。

## 实施方案

1. `ProjectTab` 的 `DragSource` 去掉 axis 限制（恢复二维拖拽）。
2. 拖的过程中经 `DragSourceEvents::Changed` 观察 `translation.y`；`Ended`
   （`DragDropResult`）里若未被任何 DropTarget 接收且 |translation.y| 超过阈值
   （建议 ≥ 48pt），触发拆窗：新窗口内容 = 该项目的工作区（请求/压测等页，
   共享进程内 store 单例，天然同一份数据），主窗口 `tabs` 移除该 id 并持久化
   `open_projects`。
3. 拖回：新窗口的标签条作为同一 typed payload（`ProjectTabDragPayload`）的
   DragSource，主窗口标签条作为 DropTarget 接收，落回后关闭次级窗口、
   `tabs` 插回对应位置。
4. 主页标签（id==0）恒不参与拆窗。

## 当前 SDK 能力与待验证项

1. **多窗口 API 已提供**：`ApplicationHandle::OpenWindow(ViewFactory, WindowOptions)`
   创建独立组合树并共享 Runtime 与应用服务。每个 factory 显式决定内容；框架不再把
   `WindowOptions` 的 key 注入 `WindowIdentity`。
2. **跨窗口拖放 API 已提供**：`DragSource::Scope(DragDropScope::Application)` 可触达同
   Runtime 的其他窗口；宿主没有原生跨窗传输时保留窗口内行为，因此 apitab 仍需保留
   松手拆窗/拖回回退路径，并在目标平台实际验证。
3. **待验证**：用户反馈的产品问题逐项补录；Linux 浮动窗口完整输入序列与
   `DragDropScope::Application` 原生传输需按 fork 当前验证记录单独确认。

## 版本检索规则

- **每次更新** HuxerUI 源码 fork pin 或 `third_party/tarballs/huxerui-sdk-*`
  （即 CMakeLists.txt 里的 SDK 版本字符串）时，检查 `include/huxerui/app.h` /
  `window.h` / `gesture.h` 与 fork 的设计、开发文档，确认窗口创建、窗口身份、
  跨窗口拖放和宿主支持边界。
- **不更新不用检索**。
- 最近检索版本：FarnaHerry/HuxerUI `farna/main` `b083028`（2026-10-29）。已具备
  应用级多窗口与 Application-scope typed drag API；窗口内容需由 `ViewFactory` 显式提供，
  `WindowIdentity` 路由与 `Retire()` 均已移除。Linux 原生输入完整序列仍有 fork 记录的待验证项。
