---
name: apitab-release
description: "维护 apitab 的发布打包、GitHub Actions、产物命名、运行库与安装器；修改工作流、packaging 或生成清理发行产物前使用。"
---

# apitab 发布与打包

操作前读取涉及的引用；本技能记录项目约定，不以通用 HuxerUI 打包示例替代。

## 必守约束

- 对外命名统一 `<os>-<arch>`：linux/macos/windows 与 x86_64/arm64；禁止 win64 等简称。
- 产物前缀 `apitab-v<version>-<os>-<arch>`；版本唯一来源是顶层 project(VERSION)。
- 每平台每发行只保留一份规范集合；同 tag 重发前先清理过时资产。
- Linux 随包带 lib/ 与 apitab.resources/，RUNPATH 使用 `$ORIGIN`，不含 CI 绝对路径。
- Windows 的 HuxerUI、OpenSSL 与桌面 CRT DLL 放 exe 旁；不递归扫构建树收 DLL。
- macOS dylib 按 .app 布局随包；系统 GUI 栈按目标平台依赖契约处理。

## 按需引用

- 改 `.github/workflows/build.yml`、`packaging/*`、命名、发行资产或运行库：
  [打包契约与检查清单](references/packaging.md)。
- 改 CI 工具链、Windows 安装器迁移、WiX/MSIX 或平台集成：
  [CI 与平台集成](references/ci-platforms.md)。
- 改源码/SDK 接入、依赖或宿主工具：
  [依赖技能](../apitab-dependencies/SKILL.md)。

完成后按根指引编译链接、运行适用检查，核对实际包内容与加载路径；
仅文件名正确不足以证明随包运行库完整。
