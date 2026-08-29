# HuxerUI 迁移地图（进行中）

本分支把前端从 EUI-NEO 0.5.7（绝对定位 DSL）整体切换到 **HuxerUI 0.1.0**
（github.com/HuxerUI/HuxerUI，组件式声明 UI：composable View + UseState +
TaskScope 结构化并发）。领域层（src/store/*、引擎、DB、config）**不变**。

## 消费方式（官方路径）

- 以官方 **预编译 SDK**（`third_party/tarballs/huxerui-sdk-0.1.0-*`，SHA256 校验）
  解包后走 `find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared)` —— 只取
  shared 组件，避免 static 分支对 gtk4/gio/libsoup dev 包的强制要求（shared 库
  运行时库由系统桌面环境提供，deb/rpm 的 find-requires 会自动声明）。
- app 目标由 **`huxerui_add_app()`** 生成（C++20 + `hcg` codegen + 资源集成）；
  领域层 apitab.* 模块在函数外用 FILE_SET 追加。**UI 层必须是普通 .cpp**：
  codegen 只扫描 .cpp/.cc/.cxx，.cppm 模块单元会被跳过。
- 官方 HuxerUI App Development skill 落库在 `.claude/skills/huxerui-app-development/`，
  写 UI 前先读（DSL 风格 / components / fundamentals / layout 等 references）。
- 框架资源包 `resources.bin` 在 POST_BUILD 拷到 `<exe>/apitab.resources/huxerui/`
  （HuxerUI 的运行时查找顺序：`HUXERUI_RESOURCES_DIR` → `<exe>.resources/`）。
- 入口：`src/app_main.cpp`（`Application{AppRoot, AppOptions}` + `RunApplication()`）。
  旧 `core::platform::requestUiUpdate` 已降级为 no-op（`app_main.cpp`）。

## 架构映射

| EUI-NEO 时代 | HuxerUI 时代 |
|---|---|
| `app::compose()` 每事件帧全量重绘 | `AppRoot()` 组合树，State 驱动失效 |
| `requestUiUpdate()` 唤醒 + poll* | `UseTaskScope` + `co_await Delay` 轮询引擎结果，直接写 State |
| ~~`store/ui.cppm` 全局视图状态~~（已删除） | 页面内 `UseState`；跨页会话走 preferences 模块（settings.ini 的 session.*） |
| 绝对定位 stack/position | Column/Row/Spacing/Padding + ScrollView |
| 自绘确认框/菜单 | 待评估（DrawerLayout / Anchored popup） |

## 已迁移（EUI 残留已全部清除）

- [x] 应用骨架：导航壳（NavigationPane：主页/请求/压测/历史/设置）+ 会话恢复
- [x] 主页：项目列表 + 打开工作区 + 刷新
- [x] 请求页（骨架）：方法/URL/发送/取消 + 响应文本（TaskScope 轮询 curl 引擎）
- [x] 删除：store/ui.cppm（旧视图状态机）、i18n 翻译表（改为 preferences 模块）、
      EUI 字体、eui-neo-compat 文档、eui-neo-ui-replicator skill、eui SDK tarball

## 待迁移（后续增强）

1. **集合侧栏**：分组树 + 右键菜单（UseMenu）→ DrawerLayout；从集合打开请求
2. **环境管理**：环境列表/变量（KvTable 模式复用）
3. **压测自动化用例**：saveAutomation/automationFromRequest 接入（从集合请求生成）
4. **本地化**：strings 资源包（resources/default.properties + namespace），替代
   已删除的 i18n 硬编码表
5. Windows SDK 的 debug dll（huxerui_debug.dll 17MB）如不需要可从 tarball 精简

## 已知差异 / 风险

- HuxerUI 0.1.0 发布于 2026-08-28（本迁移同日），API 可能随版本演进；
  vendored SDK tarball 锁定行为。
- composable 函数不要加 `inline`（hcg 改写后的函数需保持外部链接实体）；
  UI 层不要写成 .cppm 模块（会被 codegen 跳过）。
- Linux 运行时依赖 gtk4/pango/cairo/libsoup-3.0（deb/rpm 的 find-requires 会
  自动带上；预编译 SDK 的 libhuxerui.so NEEDED 即来源）。
