---
name: apitab-islands
description: apitab 自有的一级岛屿布局规范。用于修改 apitab EUI-NEO 页面、统一岛屿间距、约束容器内容和清理二级岛屿。
---

# apitab 岛屿布局规范

## 目标

apitab 使用“一级岛屿划分区块，岛内直接布局或划线分组”的视觉系统。岛屿是页面级工作区域，不是普通控件背景，也不是可以无限嵌套的卡片。

## 层级规则

- 每个主要页面区域只允许一个一级岛屿。
- 禁止在 `drawIsland(...)` 或一级岛屿内部再次调用 `drawIslandPanel(...)` / `drawIsland(...)`。
- 岛内分组使用 `Row`、`Column`、`Stack`、`.padding(...)`、`.gap(...)` 或 1px 主题分隔线。
- 输入框、按钮、segmented、选中行、表格和普通圆角背景属于控件/内容，不得升级为二级岛屿。
- 相邻工作流应合并：请求标签 + URL/操作 + 请求编辑器，压测标签 + 配置 + 输出，集合标题 + 集合列表。
- 真正独立的结果区域可以作为相邻的一级岛屿，并使用统一的细间距。

## 容器实现

- 真实岛屿必须使用 `src/ui/widgets.cppm` 的 `drawIsland(...)`。
- `drawIsland(...)` 负责背景、边框、圆角、阴影和 `.clip()`；不要用裸 `drawIslandPanel(...)` 再把页面级子元素放到外面。
- `drawIslandPanel(...)` 只适用于明确需要单独背景层、且没有未约束子内容的场景。
- 岛屿内容回调使用 EUI 标准的无参数 callback：

```cpp
drawIsland(ui, "page.section.island", x, y, w, h, theme, opacity, [&] {
    ui.text("page.section.title")
        .position(kPanelPad, kPanelPad)
        .size(nonNegative(w - 2.0f * kPanelPad), 24.0f)
        .text("标题")
        .build();
});
```

- 岛内坐标原点是岛左上角 `(0, 0)`，不是页面坐标。内容如果与页面函数共享变量，先把页面坐标折算成局部坐标。
- 岛内所有派生宽高都必须限制在岛内容框内，使用 `nonNegative(...)` / `clampToParent(...)`。
- 岛内内容不能依赖 `.clip()` 来掩盖错误几何；先保证内容布局正确，再把 `.clip()` 作为最终越界保护。
- 岛屿向上扩展覆盖外部标签条时，必须给标签条预留独立的局部高度；标签条、工具行和编辑内容不得共用同一个 y 坐标。

## 内容与滚动

- `scrollView` 必须放在岛屿内容框内，位置和尺寸扣除 `kPanelPad`。
- `scrollView` 的 content 是 column；每个逻辑行使用真实高度的 `ui.stack(rowId).size(...)` 占位。
- 变高文本使用真实测量高度，不使用 viewport 高度冒充内容高度。
- `segmented`、`dropdown`、`dataTable` 等无 position 组件必须套 positioned wrapper stack。
- 横向标签条保留独立的 clipped viewport；不要把它放进垂直 `scrollView`。

## 统一尺寸

- 岛屿之间使用 `kIslandGap`，当前为细间距 2px。
- 岛内边距使用 `kPanelPad`，不要把 `kMargin`、`kGap` 误当作岛屿外间距。
- 控件之间可继续使用语义化的 `kGap`；它不等同于岛屿间距。
- 岛屿背景统一由 `drawIsland(...)` 处理，颜色、边框、圆角和阴影不得在页面中重复定义。
- 分隔线使用 `drawHorizontalDivider(...)` 或同等 1px 主题色矩形，范围限制在岛内内容框。

## EUI 全局主题收束规则

EUI-NEO 组件会在 `.theme(tokens)` 时从主题 token 读取默认视觉值，页面局部的 `.radius(...)`、`.border(...)` 或颜色设置不能可靠地覆盖所有组件类型。后续 apitab UI 开发应优先修改项目自己的全局主题副本，而不是在页面调用点逐个打补丁。

- 所有跨页面统一的 input、button、dropdown、popup、segmented 等控件视觉值，集中配置在 `src/ui/theme.cppm` 的 `kDarkTheme` / `kLightTheme` token lambda 中。
- 例如控件圆角统一设置 `tokens.metrics.radius.card`、`tokens.metrics.radius.popup`、`tokens.metrics.radius.overlay`；不要只给某一个页面的 builder 添加局部 radius。
- 主题 token 修改只允许作用于 apitab 自己复制的 `components::theme` token，不得修改 EUI-NEO 上游默认主题或依赖库源码。
- 页面局部 `.radius(...)` 只用于语义上确实特殊的控件（例如圆形图标按钮、状态徽章、一级岛屿），并应在调用点说明理由；不能用它代替全局控件规范。
- 修改主题前先阅读 EUI 0.5.7 对应组件实现，确认组件实际读取的 token 字段（如 button 的 overlay、input 的 popup、dropdown 的 card）；不要凭字段名称猜测。
- 新增控件后检查深色/浅色主题、hover/pressed/open 状态和弹层样式，确保全局 token 在所有状态下生效。
- 若用户提出“统一圆角/边框/控件风格”，默认收束到 `src/ui/theme.cppm`，并在页面中移除不必要的重复局部样式。


- 岛屿 `.clip()` 只限制岛内内容绘制和命中范围。
- dropdown、contextMenu、确认框和其他全屏 overlay 必须保留根层组合、`.screen(...)`、原有 z-index 和 outside-dismiss 机制。
- 不要为了容器化改变稳定 ID，尤其是 tab uid、滚动状态、表格行和数据库实体 ID。
- callback 不能捕获会失效的临时引用；需要时捕获稳定 ID，在 callback 中重新读取 store 状态。

## 改动检查清单

开始修改前：

1. 搜索所有 `drawIslandPanel(` 和 `drawIsland(` 调用。
2. 标记一级区域与可能的二级区域。
3. 找到每个岛的页面原点、内容原点、实际宽高和相邻间距。
4. 先复用已有 `drawIsland`、divider、scrollView 和字段表 helper。

完成修改后：

- 确认没有一级岛屿内部的二级岛屿。
- 确认岛内所有子元素使用局部坐标，未混入页面坐标。
- 确认内容宽高没有超过岛内可用区域。
- 确认 `.clip()`、滚动条、dropdown wrapper 和 overlay z-index 正常。
- 检查宽窗口、窄窗口、空状态、长文本和滚动到底部。
- 运行 `git diff --check`、`mcpp build`；可用时运行 `mcpp build --release` 和 `./run.sh`。
- 不要规范化与本次任务无关的 `mcpp.lock` 空白行。
