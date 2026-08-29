# HuxerUI 迁移地图（进行中）

本分支把前端从 EUI-NEO 0.5.7（绝对定位 DSL）整体切换到 **HuxerUI 0.1.0**
（github.com/HuxerUI/HuxerUI，组件式声明 UI：composable View + UseState +
TaskScope 结构化并发）。领域层（src/store/*、引擎、DB、config）**不变**。

## 消费方式

- 以官方 **预编译 SDK**（`third_party/tarballs/huxerui-sdk-0.1.0-*`，SHA256 校验）
  + CMake imported target `huxerui`（shared：libhuxerui.so / libhuxerui.dylib /
  huxerui.lib + huxerui.dll）接入；不本地编译框架源码（Linux 静态库要求
  gtk4/gio/libsoup 的 dev 包，shared 库只需要运行时库，系统桌面环境自带）。
- 框架资源包 `resources.bin` 在 POST_BUILD 拷到 `<exe>/apitab.resources/huxerui/`
  （HuxerUI 的运行时查找顺序：`HUXERUI_RESOURCES_DIR` → `<exe>.resources/`）。
- 入口：`src/app_main.cpp`（`Application{AppRoot, AppOptions}` + `RunApplication()`）。
  旧 `core::platform::requestUiUpdate` 在分支上降级为 no-op（`app_main.cpp`）。

## 架构映射

| EUI-NEO 时代 | HuxerUI 时代 |
|---|---|
| `app::compose()` 每事件帧全量重绘 | `AppRoot()` 组合树，State 驱动失效 |
| `requestUiUpdate()` 唤醒 + poll* | `UseTaskScope` + `co_await Delay` 轮询引擎结果，直接写 State |
| `store/ui.cppm` 全局视图状态（Page/g_tabs/草稿） | 页面内 `UseState`；跨页会话仍走 sessionPreference |
| 绝对定位 stack/position | Column/Row/Spacing/Padding + ScrollView |
| 自绘确认框/菜单 | 待评估（DrawerLayout / Anchored popup） |

## 已迁移

- [x] 应用骨架：导航壳（NavigationPane：主页/请求/压测/历史/设置）+ 会话恢复
      （active_project）
- [x] 主页：项目列表 + 打开工作区 + 刷新
- [x] 请求页（骨架）：方法/URL/发送/取消 + 响应文本（TaskScope 轮询 curl 引擎）

## 待迁移（按优先级）

1. **请求页完整版**：Headers/Params/Body/Cookies 表格编辑（旧 request_page.cppm 861 行）
   → ForEach + TextField 网格；响应的 Headers/Body 切换（Tabs）
2. **压测页**：vus/duration 输入 + 启动/停止 + k6 实时输出流（TaskScope 循环
   drainOutput）+ LoadSummary 表格 + 自动化用例列表
3. **历史页**：分页列表（historyPage）+ 清空确认
4. **WebSocket / TCP 页**：连接/收发/事件流（引擎接口不变，UI 换成
   TextField + 事件 VirtualList）
5. **集合侧栏**：分组树 + 右键菜单 → DrawerLayout / Anchored popup
6. **设置页**：语言（i18n）/ 主题模式；**全局状态条**（g_statusMessage → State 桥）
7. 旧视图状态清理：store/ui.cppm 中 Page/g_tabs/Draft 等逐步废弃
8. Windows SDK 的 debug dll（huxerui_debug.dll 17MB）如不需要可从 tarball 精简

## 已知差异 / 风险

- HuxerUI 0.1.0 发布于 2026-08-28（本迁移同日），API 可能随版本演进；
  vendored SDK tarball 锁定行为。
- `[[huxerui::composable]]` 在 GCC 下仅是未知的 scoped attribute（忽略不报错）；
  组合约束靠运行时。
- Linux 运行时依赖 gtk4/pango/cairo/libsoup-3.0（deb/rpm 的 find-requires 会
  自动带上；预编译 SDK 的 libhuxerui.so NEEDED 即来源）。
