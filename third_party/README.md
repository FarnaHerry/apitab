# third_party — vendored 依赖

从 mcpp 迁移到 CMake 后，原本由 mcpp 注册表（`~/.mcpp/registry/data/xpkgs/`）提供
的第三方库全部 vendor 进本仓库：tarball 提交在 `tarballs/`（configure 期校验
SHA256 后解包到 `build/vendor/`，源码树不入库），nlohmann::json 是 single header
直接提交在 `json/`。构建完全离线、可复现。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.1.0 | `huxerui-sdk-0.1.0-{linux-x86_64,macos-arm64,windows-x86_64}.tar.gz/.zip` | 上游 `github.com/HuxerUI/HuxerUI` v0.1.0 官方预编译 SDK（shared 库 + headers + CMake 包 + hcg codegen 工具 + 内置资源包）。消费方式走 `find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared)` + `huxerui_add_app()`。**优先模式是源码编译**：`third_party/huxerui/`（git clone 的上游仓库，不入库）存在且依赖齐全时改为 `add_subdirectory` 编译，SDK tarball 仅作兜底；Linux 源码编译需 `gtk4-devel libsoup3-devel`，更新源码用 `cd third_party/huxerui && git pull` 后重建 |
| Asio | 1.38.1 | `asio-1.38.1.tar.gz` | 上游 `chriskohlhoff/asio` tag asio-1-38-1，同 `chriskohlhoff.asio` |
| IXWebSocket | 12.0.1 | `ixwebsocket-12.0.1.tar.gz` | 上游 `machinezone/IXWebSocket` v12.0.1，同 `compat.websocket`（client-only、无 TLS/无 zlib，32 个源文件） |
| curl | 8.21.0 | `curl-8.21.0.tar.gz` | 上游 `curl/curl` release tarball，同 `compat.curl` |
| SQLiteCpp | 3.3.3 | `SQLiteCpp-3.3.3.tar.gz` | 上游 `SRombauts/SQLiteCpp` v3.3.3（内置 sqlite3 amalgamation），同 `compat.sqlitecpp` |
| OpenSSL | 3.5.1 | `openssl-3.5.1-linux-x86_64.tar.gz` | `compat.openssl` 管线构建的**静态预编译产物**（libssl.a/libcrypto.a + include），仅 linux x86_64 兜底；其他平台用系统 OpenSSL（ubuntu libssl-dev / macos brew openssl@3 / windows vcpkg） |
| nlohmann::json | 3.12.0 | `json/nlohmann/json.hpp`（single header） | 上游 `nlohmann/json` v3.12.0 `single_include` |

## 为什么提交预编译的 OpenSSL

本机（Fedora）没有 openssl-devel 且 `import std` 的构建无法依赖 sudo 装包；
asio::ssl 与 curl https 又必须链接 OpenSSL。因此把原 compat.openssl 管线产出的
静态库原样收进 tarball，仅作为"系统找不到 OpenSSL 时"的 linux x86_64 兜底
（`third_party/CMakeLists.txt` 顶部的查找顺序）。若你愿意装系统包，可直接删掉
这个 tarball。

## 更新某个依赖

1. 用新版本源码打 tarball（保持顶层目录名，或同步改 `third_party/CMakeLists.txt`
   里 `apitab_extract` 的 `topdir` 参数）；
2. `sha256sum` 新值写回 `third_party/CMakeLists.txt`；
3. 跑一次 configure 验证解包与 SHA 校验。
