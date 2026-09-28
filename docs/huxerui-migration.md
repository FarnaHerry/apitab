# HuxerUI 项目规范与现状

本项目 UI 基于 HuxerUI 0.3.0。此文记录当前接入方式和本仓约定，避免沿用早期迁移
阶段的线程、State 与 SDK 说明。

## 接入与文件布局

- CMake 当前优先编译 `third_party/huxerui` 源码；`HUXERUI_HOME` 可指定源码目录或
  已安装的 0.3.0 SDK，Linux 离线 SDK 也锁定为 0.3.0。SDK 和源码包含的 API 可能不同，
  顶层 CMake 会检查实际头文件并选择兼容实现。
- `huxerui_add_app()` 负责 HuxerUI codegen 与应用资源。composable 放在普通 `.cpp`
  中；不要把 UI 文件写成 `.cppm`，也不要给 composable 添加 `inline`。
- `src/app.cpp` 声明 HuxerUI `Application`；平台入口在
  `platform/<platform>/main.cpp`；应用根 composable 和导航壳在 `src/ui/app.cpp`。
- HuxerUI 合并资源包生成在 `build/huxerui-resources/apitab/package/`，构建后复制到
  `<exe>.resources/huxerui/resources.bin`。

## 当前架构映射

| 项目职责 | HuxerUI 用法 |
|---|---|
| 页面组合 | composable 返回 `huxerui::View`，由 State 驱动重组 |
| 页面与标签切换 | `IndexedPages` 加稳定 `Key`，保留未选中页面的状态 |
| 动态大列表 | `StateList` 配合 `VirtualList`；动态兄弟项提供稳定 `Key` |
| 引擎结果 | UI 协程用 `PollWhile` 定时轮询 store，并在 UI 线程更新 State |
| 阻塞或 CPU 工作 | `co_await huxerui::RunWorker(fn)`，恢复后在 UI 协程更新 State |
| 外部线程回调 | 用 `TaskScope::Post` 投递到 UI 线程 |
| 主题 | 在 `MaterialTheme` 内读取现有 typed style，再只覆写品牌字段 |

## 编写约定

1. **组合阶段保持纯。** 不要在 composable 组合时赋值 State。初值先计算再传给
   `UseState`；需在挂载后同步外部值时使用 `Lifecycle`。
2. **事件处理器可同步更新 UI State。** HuxerUI 会在 handler 返回后处理失效和重组，
   纯导航、选中项和标签状态更新直接在 UI handler 内完成，不要仅为推迟这些操作而套
   `TaskScope::Launch` + `Delay(0)`。可能阻塞的存储或计算工作交给 `RunWorker`。原生窗口
   close/hide 若会在系统回调栈内销毁窗口，则沿用 `src/ui/app_dialogs.cpp` 和
   `src/ui/app.cpp` 的延后处理。
3. **Worker closure 只做后台工作。** 只捕获所需的普通值，不读写 HuxerUI `State`，
   不创建或操作 `View`。结果经 `co_await RunWorker` 返回后，再在 UI 协程写 State。
   引擎自己的后台队列仍由引擎持有；无需再建应用级通用线程池。
4. **样式从当前主题继承。** 在 `MaterialTheme` 范围内通过
   `UseEnvironment<Style>()` 取得当前完整组件样式，再覆写产品确实需要的属性；不要
   用 `Style::Default()` 抹掉 Material 默认行为。
5. **遵循 View 值语义。** API 按值接收 `View`。把有名 View 作为容器 child 或参数传入时
   直接传值；只有 fluent builder 要求右值时才对局部 View 使用 `std::move`。
6. **取消路径要安全。** 页面卸载会取消其 `TaskScope` 协程。协程持有的临时文件等资源
   必须由协程帧中的 RAII guard 同步清理，不能只依赖 `co_await` 后或函数末尾的清理语句。

完整分层、线程及资源所有权规则见 [`architecture.md`](architecture.md)；详细 API 规则见
仓库内 `.claude/skills/huxerui-app-development/SKILL.md` 和对应 references。
