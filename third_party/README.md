# third_party — vendored 依赖

从 mcpp 迁移到 CMake 后，原本由 mcpp 注册表（`~/.mcpp/registry/data/xpkgs/`）提供
的第三方库全部 vendor 进本仓库：tarball 提交在 `tarballs/`（configure 期校验
SHA256 后解包到 `build/vendor/`，源码树不入库），nlohmann::json 是 single header
直接提交在 `json/`。构建完全离线、可复现。

## 清单与来源

| 包 | 版本 | tarball | 来源 |
|----|------|---------|------|
| HuxerUI | 0.2.0 | `huxerui-sdk-0.2.0-linux-x86_64.tar.gz` | 由官方 0.2.0 SDK 安装前缀归档（shared 库 + headers + CMake 包 + hcg/hrc + 内置资源）。`HUXERUI_HOME` 可指向 0.2.0 SDK 安装目录或源码根目录；未设置时优先 `third_party/huxerui/` 源码，`APITAB_HUXERUI_FORCE_SDK=ON` 时使用 Linux 离线包。Linux 源码模式需 `gtk4-devel libsoup3-devel`；macOS/Windows 必须通过 `HUXERUI_HOME` 提供 0.2.0 源码或 SDK。 |
| Asio | 1.38.2 | `asio-1.38.2.tar.gz` | 上游 `chriskohlhoff/asio` tag asio-1-38-2，同 `chriskohlhoff.asio` |
| IXWebSocket | 12.0.1 | `ixwebsocket-12.0.1.tar.gz` | 上游 `machinezone/IXWebSocket` v12.0.1，同 `compat.websocket`（client-only、无 TLS/无 zlib，32 个源文件） |
| curl | 8.22.0 | `curl-8.22.0.tar.gz` | 上游 `curl/curl` release tarball，同 `compat.curl` |
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
