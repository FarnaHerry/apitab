# SweetEdit 接入与补丁

修改编辑器接入、升级 SweetEdit 或嵌套依赖前读本文件。
补丁是否仍需保留，应按当前上游检查；不要仅按这里记录的历史提交重放。

- sweetedit 接入：`third_party/huxerui-sweetedit`（git clone，不入库，已
  gitignore）提供 SweetEditor 代码编辑器（行号/语法高亮），经
  `huxerui_use_library(apitab TARGET sweetedit_core ...)` 编译；其
  3dparty/SweetEditor、SweetLine 是 gitlink 但上游无 .gitmodules，真实源为
  github.com/FinalScave/SweetEditor(bd4330e)、FinalScave/SweetLine(64665d9)，
  已手动 clone 固定提交。已用于请求页 body 编辑器（request_page.cpp
  BodyTextEditor）和压测页 k6 脚本编辑器（loadtest_page.cpp）；语法 DSL 在
  src/ui/syntax_grammars.h（kJsonSyntax/kJavaScriptSyntax/kXmlSyntax/
  kGraphqlSyntax/kPlainSyntax）。body 文本按类型独立存档（RequestDraft::bodies
  下标 = api::BodyKind 值，保存时全量落 db::SavedRequest::bodyContents）。
  注意：SweetEditor **非受控组件**，外部改文本必须
  `controller.LoadDocument(key, text, syntax)`，仅 document_key 变化才重载
  initial_text。本地补丁（改动处均带 `NOTE(apitab local patch)` 注释，
  **上游 pull 后需逐一核对/归位**）：① `include/sweetedit_core/sweet_editor.h`
  的 11 个事件声明从旧式 `Event<Args...>`（面向早期 SDK）改为 HuxerUI
  main 的函数签名式 `Event<void(Args...)>`，发射侧不受影响；②
  `src/sweet_editor/sweet_editor.cpp` 的 8 处 StrokePath/DrawBorder 由
  float 线宽改为 `StrokeStyle{.width=...}`、`Extension::OnKey` 返回类型
  void→bool（跟进 81e8bbc 统一描边样式、a7d38c9 可消费键盘路由）；
  ③ **调色板注入**：新增 `SweetEditorPalette`（约 60 个 ARGB 字段，
  默认=原浅色）+ `SweetEditorOptions::palette`，sweet_editor.cpp /
  sweetline_highlighter.cpp 全部颜色（含语法高亮与彩虹括号）改读 palette。
  所有 `0x........` ARGB 字面量一律包 `static_cast<int32_t>(...)`：MSVC
  `/std:c++latest` 按 C++26 规则把常量表达式里的非值保留整型转换判为窄化
  （error C2397），windows-x86_64 CI 实证编译失败。
  应用侧经 `EditorPalette(theme)`（common.cpp，深色=结构色从 ThemeSpec
  派生 + VS Code Dark+ 语法色）在 BodyTextEditor 与压测页脚本编辑器接线。
  ④ `codeeditor.cpp` 的滚轮桥接经 CMake 头文件特性检测兼容旧 `ScrollEvent`
  与新 `ScrollInputEvent`；并适配新版 `TextSelectionClient` 的
  `ClearSelection` / `TextSelectionGeometry` 和 `NodeExtension::OnFocusChanged`
  的 `reverse` 参数。该组合以更新后的 HuxerUI 源码通道为基线；补丁
  `cmake/patches/huxerui-sweetedit-scroll-event.patch` 由 CI 在主补丁后应用。
  ⑤ **嵌套 SweetEditor 的 os_log GCC 兼容**（`cmake/patches/sweeteditor-oslog-gcc.patch`）：
  `logging.hpp` 的 `#elif defined(__APPLE__)` 分支 `#include <os/log.h>` 依赖
  clang-only 的 `__builtin_os_log_format`，改为 `defined(__APPLE__) && defined(__clang__)`
  （GCC 落 iostream 兜底）。macOS 通道已整体切到 brew llvm clang/libc++（见文末
  CI 备注），正常走 os_log；补丁保留作防御，防有人拿 GCC 编 __APPLE__ 下的
  SweetEditor。CI 经 sweetedit CMakeLists 的 FetchContent PATCH_COMMAND 应用（幂等），
  本地手动 clone 的 3dparty/SweetEditor 必须同样应用保持 parity；嵌套 pull 新提交
  后需重放并复跑 `git apply --check`。
  ⑥ **嵌套 SweetLine 的 wstring_convert 头文件修复**（`cmake/patches/sweetline-locale-include.patch`）：
  `util.cpp` 一直靠 `<codecvt>`/`<filesystem>` 的传递包含拿到 `std::wstring_convert`
  （标准归属头是 `<locale>`），brew llvm 22→23 滚动更新后 libc++ 23 剪掉了传递链，
  macOS 通道直接编译失败；补丁显式补 `#include <locale>` 与 `#include <algorithm>`
  （util.cpp 的 std::copy 同理）。CI 经 sweetedit CMakeLists 的 sweetline
  FetchContent PATCH_COMMAND 应用（幂等），本地手动 clone 的 3dparty/SweetLine
  必须同样应用保持 parity。
  ⑦ **只读编辑器复制失效修复**（`cmake/patches/huxerui-sweetedit-selection-client.patch`，
  已提交 Lib-CodeEditor fork 870a82f，上游跟进后删除）：`EditorBehavior`
  扩展的 `GetTextSelectionClient()` 原本硬编码返回 `nullptr`（意图是避免窗口级
  选择覆盖层叠画），但只读编辑器永远没有 IME 会话（框架
  `RefreshTextInputSession` 对 read_only 配置直接不建会话），Ctrl+C/全选只能
  经"焦点节点的 TextSelectionClient"路由，返回 nullptr 使复制整链失效（右键
  菜单"复制"的剪贴板指针也只在框架路由时注入，同样为 null）。修复 = 返回
  holder 持有的真实 client；`QuerySelectionGeometry` 返回 nullopt 已足够让
  窗口覆盖层保持隐藏，视觉零变化。回归测试 `tests/test_editor_copy.cpp`
  （TestPlatform + 假剪贴板，复刻响应区只读编辑器拖选 + Ctrl+C）。
  均建议反馈作者：sweetedit 需跟进 HuxerUI main 的 Event/Stroke/键盘
  API 变化，并支持官方主题配色入口；os_log 路径应显式门控 __clang__。
