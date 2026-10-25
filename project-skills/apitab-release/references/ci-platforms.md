# CI 与平台集成

修改 CI、平台运行库、安装器或编译器组合前读本文件。
以下承接根指引的迁移记录；实际版本与步骤以工作流和 CMake 为准。

- Windows：HuxerUI 运行时 dll 在 POST_BUILD 拷到 exe 旁；OpenSSL 走 chocolatey，
  CI 里以 `OPENSSL_ROOT_DIR` 注入（共享库，其 `libssl-N-x64.dll`/`libcrypto-N-x64.dll`
  与 MSVC 桌面 CRT `msvcp140/vcruntime140*` 同样在 POST_BUILD 拷到 exe 旁——干净
  Windows 三者都没有；CI staging 只收 exe 旁的 dll 并断言齐全，不递归扫构建树，
  避免卷入预置宿主工具的 .NET/WebView2 程序集和 `*_app` UWP CRT 变体）。
- CI 的 linux job 用 ubuntu:26.04 容器 + clang-21/libc++；macOS job 整链 brew
  llvm clang/libc++（.mm 框架层只能 clang 编译，std 类型贯穿 .mm/.cpp 接口 →
  单进程必须单一标准库；此前的 brew GCC 混链 AppleClang 在 mangling 分叉上直接
  链接失败）。`import std` 的 libc++.modules.json 由 CI 按上游 schema 生成进
  brew llvm 的 lib/，configure 经 `CMAKE_CXX_STDLIB_MODULES_JSON` 显式注入
  （brew formula 不开 LIBCXX_ENABLE_STD_MODULES）。详见
  `.github/workflows/build.yml` 注释。

- 版本号唯一来源是顶层 `project(apitab VERSION ...)`；packaging 脚本与
  Windows WiX 步骤从 CMakeLists 解析。Windows 安装器为 WiX Burn bundle
  （perMachine, Program Files）；≤0.1.3 曾用 NSIS（per-user，
  %LOCALAPPDATA%\Programs\apitab，apitab.nsi 已删除）——MSI 的 Package.wxs
  后处理（build.yml）在安装时清理 NSIS 遗留目录/卸载键/快捷方式，用户数据
  （%APPDATA%\apitab）不在此列。另有 MSIX 通道：`platform/windows/msix/`
  清单 + `microsoft/setup-WinAppCli` + `winapp pack`（CI 随机自签，.cer 随
  包；staging 双 exe 必须 `--exe apitab.exe` 指定入口）。
