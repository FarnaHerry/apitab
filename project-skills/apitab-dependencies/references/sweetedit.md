# SweetEdit 接入与补丁

修改编辑器接入、升级 SweetEdit 或嵌套依赖前读本文件。

- sweetedit 接入：`third_party/huxerui-sweetedit`（git clone，不入库，已
  gitignore）提供 SweetEditor 代码编辑器（行号/语法高亮），经
  `huxerui_use_library(apitab TARGET codeeditor ...)` 编译（上游已把
  sweetedit_core 重命名为 codeeditor）。clone 源为 **FarnaHerry/Lib-CodeEditor
  fork 的 farna/main**（2026-11-10 起 CI 与本地 dev 同源，pin
  `a332e7c3158818a891bb8181756d439a62df6268`；官方 repo 为 HuxerUI/Lib-CodeEditor，
  原 huxerui-sweetedit 名字经 GitHub 重定向仍可用）。fork 含全部 apitab 本地
  功能补丁与后续修复（剪贴板可靠性、worker 预解析文档、simdutf/库图构建修复）。
- 已用于请求页 body 编辑器（request_page.cpp BodyTextEditor）和压测页 k6 脚本
  编辑器（loadtest_page.cpp）；语法 DSL 在 src/ui/syntax_grammars.h
  （kJsonSyntax/kJavaScriptSyntax/kXmlSyntax/kGraphqlSyntax/kPlainSyntax）。
  body 文本按类型独立存档（RequestDraft::bodies 下标 = api::BodyKind 值，保存时
  全量落 db::SavedRequest::bodyContents）。
  注意：SweetEditor **非受控组件**，外部改文本必须
  `controller.LoadDocument(key, text, syntax)`，仅 document_key 变化才重载
  initial_text。
- 嵌套 `3dparty/SweetEditor`（bd4330e）、`3dparty/SweetLine`（64665d9）是
  gitlink；fork 已带 `.gitmodules`（gitlink 与上面 pin 一致），递归 clone 可得，
  但 apitab/CI 仍按固定 commit 显式 clone。两个防御性补丁保留在
  `cmake/patches/`（fork `3dparty/patches/` 留有同内容副本）：
  ① `sweeteditor-oslog-gcc.patch`：`logging.hpp` 的 os_log 路径依赖 clang-only
  builtin，门控为 `__APPLE__ && __clang__`（GCC 落 iostream 兜底）；macOS 通道
  走 brew llvm clang 正常用 os_log，补丁防 GCC 编 __APPLE__。CI 经 sweetedit
  CMakeLists 的 FetchContent PATCH_COMMAND 应用（幂等），本地手动 clone 必须
  同样 `git apply` 保持 parity。
  ② `sweetline-locale-include.patch`：`util.cpp` 靠传递包含拿
  `std::wstring_convert`/`std::copy`，libc++ 23 剪掉传递链后显式补
  `<locale>`/`<algorithm>`。
- **历史 apitab 补丁已全部收编进 fork 并删除**（FetchContent 兜底、行号右对齐、
  分隔线读主题色、sweetline_provider 日志静音、新旧 `ScrollEvent`/滚动处理器
  桥接、响应区滚动、只读编辑器 `GetTextSelectionClient` 复制修复、
  `MountedNode`→`ViewNode` 重命名、`ContextMenuEntry::label` 悬空修复）；
  不要恢复旧补丁或 `cmake/patches/huxerui-sweetedit-*.patch` 文件。调色板注入
  （`SweetEditorPalette` + `SweetEditorOptions::palette`，应用侧经
  `EditorPalette(theme)` 接线）同样由 fork 维护。
- 回归测试 `tests/test_editor_copy.cpp`（TestPlatform + 假剪贴板，复刻响应区
  只读编辑器拖选 + Ctrl+C）覆盖只读复制链路；升级 fork 后须跑。
- 均建议反馈作者：os_log 路径应显式门控 `__clang__`（补丁内容适合上游化）。
