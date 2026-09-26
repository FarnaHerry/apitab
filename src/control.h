// control.h — 控制面**客户端半边**（薄客户端：端点发现 + 命令往返）。
//
// 服务端半边（起 loopback 服务、应用线程投递口）在 control_server.h——那份依赖
// huxerui；客户端不依赖，所以能单独编进测试目标（tests/test_control_client.cpp）。
//
// 形态见 docs/plans/runtime-control-surface.md：DB 与 store 的唯一属主是 **GUI 进程**，
// `apitab --cli …` 只是运行中实例的薄客户端——命令在实例的**应用线程**上执行，
// 结果原样回传。
//
// 端点：127.0.0.1 上的一次性端口 + 每次启动重新生成的随机 token；两者写进
// `$XDG_RUNTIME_DIR/apitab-control.json`（POSIX，0600；Windows 回落数据目录），
// 进程退出即删除。只绑回环、只认带 token 的请求。
#pragma once

#include <string>
#include <vector>

namespace apitab::control {

// 端点文件路径（缺失目录会被创建）。
std::string EndpointFilePath();

// 读取端点文件里的 {port, token}；文件缺失/损坏 → false，error 给出可读原因。
bool ReadEndpoint(int& port, std::string& token, std::string& error);

// 实例是否已在运行（端点文件存在且能连上）。
bool InstanceRunning();

// 拉起实例并等端点就绪（默认路径见 ForwardCommand 的 ensure）。
// 注意：HuxerUI 目前没有"启动即隐藏"，所以拉起的实例会先显示窗口（上游 TODO：
// WindowOptions::start_hidden，见 docs/plans/runtime-control-surface.md §8）。
bool StartInstanceAndWait(std::string& error);

// help 类请求（无参数 / help / --help / -h，含子命令 --help）：纯文本、不需要实例。
// 由平台入口在转发前判定并本地调用 cli::run，这样客户端半边不依赖 cli（便于单测）。
bool IsHelpOnly(const std::vector<std::string>& args);

// 一条命令的往返结果（不打印）：供 ForwardCommand 与测试使用。
struct CommandResult {
    int exit_code = 1;
    std::string stdout_text;
    std::string stderr_text;
};

// 把一条命令发给运行中的实例并取回结果。失败返回 false 并填充 error。
bool ExchangeCommand(const std::vector<std::string>& args, CommandResult& result,
                     std::string& error);

// 把一条命令转发给运行中的实例：命令在实例的应用线程执行，stdout/stderr 原样回放
// 到本进程标准流，返回它的退出码。连接失败时返回 1 并填充 error（调用方负责打印）。
// ensure=true 时：实例没在跑就先拉起并等端点就绪（`apitab --cli --ensure …`）。
int ForwardCommand(const std::vector<std::string>& args, std::string& error, bool ensure = false);

} // namespace apitab::control
