# 计划：项目标签拖出成独立窗口（tear-off）与拖回

状态：**阻塞于 SDK 能力**，未实施。代码侧已留标记（`src/ui/app.cpp` ProjectTab 拖拽注释）。

## 需求

- 顶级项目标签（`ProjectTabStrip`）当前限定 X 轴拖拽换位
  （`DragGesture{.axis = Axis::Horizontal}`，app.cpp / request_page.cpp 均已锁定）。
- 目标交互：项目标签竖向（Y 轴）拖出标签条超过阈值时，把该项目的工作区
  拆成一个**独立 OS 窗口**；独立窗口里的标签可以**拖回**主窗口的项目标签条。
- 仅项目级标签生效；请求页内部标签（RequestTabStrip chips）不需要此功能，
  保持纯 X 轴换位。

## 实施方案（SDK 支持后）

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

## 阻塞点（需要 HuxerUI SDK 提供）

1. **多窗口 API**：运行期创建/销毁次级窗口（如
   `ApplicationHandle.OpenWindow(WindowOptions, RootFactory)`）。0.1.0 只有
   单 `Application` 单 root factory，`UseWindow()` 仅主窗口命令。
2. **跨窗口拖放**：同进程 typed payload 拖拽会话跨窗口边界（A 窗口
   DragSource → B 窗口 DropTarget）；或至少提供「拖拽离开窗口边界」回调
   （带最终屏幕坐标），由应用层自建 tear-off。

## 版本检索规则

- **每次更新** `third_party/tarballs/huxerui-sdk-*`（即 CMakeLists.txt 里的
  SDK 版本字符串变化）时，检索新版 SDK 是否提供上述两项能力（查
  `include/huxerui/app.h` / `window.h` / `gesture.h` 与官方 changelog）。
- **不更新不用检索**。
- 最后检索版本：源码 main `4a56daf`（2026-08-31；两项均不支持。此前
  0.1.0 时 PointerEvent 无 button 字段，现已落地并另有
  `ViewEvents::ContextMenuRequested`）。
