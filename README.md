# apitab

apitab 是一个使用 C++23 和 HuxerUI 构建的桌面 API 开发工具，面向接口调试、请求管理、
测试用例、Mock、历史记录和负载测试等工作流。项目同时提供无界面的 CLI，可与 GUI
共用本地数据并用于脚本或 agent 自动化。

## 功能

- 按组织和项目管理接口、目录与请求标签
- HTTP 请求编辑：Params、Headers、Cookies、Body 和环境变量
- 环境管理、基础 URL 拼接与 `{{variable}}` 变量替换
- 请求历史、测试用例和 Mock 响应
- WebSocket、TCP、HTTP 测试与 k6 负载测试入口
- 浅色/深色主题、顶级项目标签和自定义桌面窗口壳
- CLI 查询项目数据、查看请求、发送请求并读取历史

项目仍在开发中，界面和数据结构可能继续调整。

## 构建要求

- CMake 3.30 或更高版本
- 支持 C++23 modules / `import std` 的编译器
- Ninja（推荐）
- Linux 源码构建需要 GTK 4 与 libsoup 3 开发包

Fedora：

```bash
sudo dnf install cmake ninja-build gcc-c++ gtk4-devel libsoup3-devel
```

HuxerUI 按以下顺序选择：显式 `HUXERUI_HOME` 源码、
`third_party/huxerui` 源码、已安装 SDK、仓库内离线 SDK。默认采用 source-first，便于
开发期间持续跟进 HuxerUI 最新源码。

## 构建与运行

直接使用 CMake：

```bash
cmake -S . -B build -G Ninja
cmake --build build --target apitab
./build/apitab
```

也可以使用 HuxerUI CLI：

```bash
huxerui build linux
huxerui run linux
```

强制验证预编译 SDK 通道：

```bash
cmake -S . -B build-sdk -G Ninja -DAPITAB_HUXERUI_FORCE_SDK=ON
cmake --build build-sdk --target apitab
```

## 测试

```bash
cmake --build build --target test_smoke
ctest --test-dir build --output-on-failure
```

测试包含基础冒烟检查与 HuxerUI 来源选择矩阵。

## CLI

CLI 不启动图形界面，并与 GUI 共用 SQLite 数据和当前项目上下文：

```bash
./build/apitab --cli help
./build/apitab --cli orgs
./build/apitab --cli projects
./build/apitab --cli requests --project 1
./build/apitab --cli show 1 --project 1
./build/apitab --cli send 1 --project 1 --json
./build/apitab --cli history --limit 10
```

完整说明见 [docs/apitab-cli.md](docs/apitab-cli.md)。

## 数据位置

Linux 默认数据目录为：

```text
~/.local/share/apitab
```

GUI 与 CLI 共用其中的 `apitab.db` 和 `settings.ini`。操作真实数据前建议先备份该目录。

## 项目结构

```text
src/          业务、数据层、CLI 与 UI
platform/     Linux、Windows、macOS 等平台入口
resources/    图片、字符串和应用资源
tests/        冒烟测试与构建选择测试
docs/         CLI、迁移记录和开发计划
third_party/  依赖、HuxerUI 源码或离线 SDK
```

## 许可证

[MIT](LICENSE)
