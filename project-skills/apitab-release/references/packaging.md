# apitab 发布 / 打包规范

**触发方式**：`main` 推送、PR、`v*` tag 都触发（另有手动 `workflow_dispatch`）。
本仓库是**公开仓库**，标准 GitHub-hosted runner（ubuntu/windows/macos）不计费，所以
日常推送照跑全平台构建；`v*` tag 额外跑打包 + GitHub Release（release job 由
`startsWith(github.ref, 'refs/tags/')` 守卫）。发布提交（提交信息以
`chore(release): prepare ` 开头）的 main 推送会被守卫跳过，交给同一提交的 tag run，
避免同一棵树重复编译一遍。

本仓库经 GitHub Actions（`.github/workflows/build.yml`）构建、打包并发布。所有面向
用户的**产物命名**、CI **矩阵/job/artifact 命名**必须统一用全称组合 `<os>-<arch>`，
**禁止简称**（如 `win64`/`win`/`x64`/`amd64`/`mac`/`osx` 出现在对外文件名里）。

## 1. 平台 / 架构 token（唯一合法值）

| 维度 | 合法值 | 说明 |
|---|---|---|
| `<os>` | `linux` / `macos` / `windows` | 全小写，**不许** `win`/`mac`/`osx` |
| `<arch>` | `x86_64` / `arm64` | 全小写，**不许** `x64`/`amd64`/`win64`/`aarch64` |

> deb/rpm 内部的 `Architecture` 字段按各自工具链命名（dpkg=`amd64|arm64`、
> rpm=`x86_64|aarch64`）是**内部**映射；对外文件名仍用上表 token，二者分开维护。

## 2. 发布产物命名（以 tag/version = `v<semver>`，来自 project(VERSION) 与 git tag）

统一前缀：`apitab-v<version>-<os>-<arch>`，后缀按格式/用途。

| 平台 | 产物 | 示例 |
|---|---|---|
| linux | `.tar.gz` / `.deb` / `.rpm` | `apitab-v0.1.0-linux-x86_64.tar.gz` |
| macos | `.tar.gz` | `apitab-v0.1.0-macos-arm64.tar.gz` |
| windows | `.zip` + `-setup.exe`（WiX Burn bundle：MSI + Installer UI）+ `.msix`/`.cer`（winapp CLI，自签） | `apitab-v0.1.0-windows-x86_64.zip`、`apitab-v0.1.0-windows-x86_64-setup.exe`、`apitab-v0.1.0-windows-x86_64.msix` |

- 一个平台同一发行只产**一份规范集合**，不要历史遗留 / 重命名副本并存。
- 对**同一个已发布 tag 重新发布**时（force 移 tag / 重跑 release），
  softprops/action-gh-release 只追加不清理 → 旧命名产物会残留成“简称 + 全称”混杂。
  重发前先用 `gh api` 删掉过期资产（例如历史残留的 `...-win64.zip`）。

## 3. 命名在各文件的落点（改动时全部保持同源）

- `packaging/ci-package.sh`：`apitab-v$version-$os-$arch.tar.gz`（linux/macos）。
- `packaging/ci-package.ps1`：`apitab-v$version-windows-x86_64.zip`。
- Windows setup.exe：`.github/workflows/build.yml` 的 WiX 步骤从 `project(VERSION)`
  解析版本（bundle 名 `apitab-Setup-$version.exe` → 复制为规范名）。曾用的
  `packaging/apitab.nsi` 已删除（NSIS per-user 安装是“装新不删旧”遗留的源头；
  MSI 内建 NSIS 遗留清理，见 build.yml 的 Package.wxs 后处理段）。
- Windows MSIX：`platform/windows/msix/Package.appxmanifest`（Identity
  FarnaHerry.apitab / CN=FarnaHerry 固定，`@MSIX_VERSION@` 占位）经
  `microsoft/setup-WinAppCli` + `winapp pack` 打包签名；**每次发布随机自签
  证书**，`.cer` 随包（安装方导入「受信任的人」存储）；后续固定证书改用
  repo secret。staging 有 apitab.exe + k6.exe 两个 exe，pack 必须 `--exe
  apitab.exe` 显式指定入口。
- `.github/workflows/build.yml` 的 `package` 矩阵 name 与 `upload-artifact` name：
  `build-<os>-<arch>` / `pkg-<os>-<arch>`（如 `build-linux-x86_64`）。
- release job 经 `softprops/action-gh-release` 用 `files: artifacts/*` 上传 `apitab-v*`。

## 4. 随包运行库（runtime bundle）不变量

apitab 是动态链接 HuxerUI（源码通道按 shared 编译）的 GUI，release 包必须自洽：

- **Linux**：构建阶段已把 `lib/libhuxerui.so`、`libc++.so.1`/`libc++abi.so.1`（含
  `libunwind.so.1` 若存在）与 `apitab.resources/huxerui/resources.bin` 放进产物，
  并用 `patchelf --set-rpath '$ORIGIN/lib'` 覆盖 RUNPATH（**严禁把构建机绝对路径
  烘焙进 RUNPATH**）。`packaging/ci-package.sh` 在 linux 分支把 `lib/`、
  `apitab.resources/` 拷进发布目录；deb/rpm 脚本同样装入 `/opt/apitab`。
- **Windows**：`huxerui.dll`、OpenSSL dll（`libssl-N-x64.dll`/`libcrypto-N-x64.dll`）与
  MSVC 桌面 CRT（`msvcp140.dll`/`vcruntime140.dll`/`vcruntime140_1.dll`）需与 exe
  同目录随包分发。三者统一由 CMake POST_BUILD 拷到 exe 旁（OpenSSL dll 从
  `<OPENSSL根>/bin` glob、CRT 从 vcvars 的 `VCToolsRedistDir` 定位），CI staging 只收
  exe 旁的 dll 并对上述清单断言——构建树递归扫 dll 会把预置宿主工具的 .NET/WebView2
  程序集和 `*_app`（UWP）CRT 变体扫进来，那些 apitab 从不加载。
- **macOS**：`.app` 内的 huxerui dylib 需随包（按 app bundle 布局）。
- 系统 GUI 栈（Linux 的 gtk4/libsoup、X 等）作为**外部依赖**在 deb `Depends` 声明、
  rpm 由 `AutoReqProv` 自动生成，不随包内嵌。

## 5. 改动的自查清单

- [ ] 文件名只用 `<os>-<arch>` 全称，无 `win64`/`x64`/`amd64` 等简称。
- [ ] 一个平台一份规范产物，无陈旧/重复资产（同 tag 重发前先清旧）。
- [ ] RUNPATH 用 `$ORIGIN/...`，不含 `/__w/...` 等 CI 工作区路径。
- [ ] `lib/` 与 `apitab.resources/` 已进 tar/deb/rpm（Linux）；exe 旁带 dll（Windows）。
- [ ] deb `Depends` / rpm `AutoReqProv` 反映真实系统依赖。
