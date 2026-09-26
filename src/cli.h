// cli.h — apitab 命令行模式（`apitab --cli <子命令>`）。
//
// 形态（阶段 0 起，见 docs/plans/runtime-control-surface.md）：
// `apitab --cli …` 是**运行中实例的薄客户端**——命令真正在 GUI 进程的应用线程上执行，
// 结果经控制面回传。命令实现只有这一份（下面的 run 重载），所以 stdout/stderr 分流、
// 退出码、--json 形状对客户端与进程内调用完全一致。
//
// 旧的"CLI 自己开库的无头进程"方向已废弃：它既打不过 curl，又把 CLI 变成唯一没有
// HuxerUI Runtime 的 DB 使用者（ORM 的 Task 在无 Runtime 进程里不会恢复）。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace apitab::cli {

// 命令输出汇：stdout/stderr 分流。进程内直跑写标准流；控制面服务端把它收集成
// 字符串随响应回传。
struct Sink {
    virtual ~Sink() = default;
    virtual void Out(std::string_view line) = 0;
    virtual void Err(std::string_view line) = 0;
};

// 进程入口：写 std::cout / std::cerr。
int run(const std::vector<std::string>& args);

// 控制面入口：输出进 sink（调用方负责串行化——命令实现在应用线程上执行）。
int run(const std::vector<std::string>& args, Sink& sink);

} // namespace apitab::cli
