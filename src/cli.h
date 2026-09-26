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

#include <chrono>
#include <memory>
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

// ---- 发送的三段式（控制面专用）------------------------------------------------
// send 是唯一会**长时间等待**的命令（传输最长 120s）。若整条命令都跑在应用线程上，
// GUI 会在整段传输里冻结。所以拆成三段：
//   BeginSend   —— 应用线程：读 store、finalizeSpec、入队引擎（纯入队，立即返回）
//   WaitSend    —— 任意线程（控制面用 IO 线程）：只读引擎结果槽，不碰 store
//   FinishSend  —— 应用线程：Cookie 归集、落历史、输出与退出码
// 不透明句柄：模块类型（RequestSpec/SavedRequest）不出现在这个普通头里。
class SendHandle;

// 返回空句柄表示失败（已写入 sink，exitCode 为退出码）。必须应用线程调用。
std::shared_ptr<SendHandle> BeginSend(const std::vector<std::string>& args, Sink& sink,
                                      int& exitCode);
// true = 拿到结果（或 mock 完成）；false = 超时。
bool WaitSend(const std::shared_ptr<SendHandle>& handle, std::chrono::seconds timeout);
// 必须回到应用线程调用；返回命令退出码（0/1/2）。
int FinishSend(const std::shared_ptr<SendHandle>& handle, Sink& sink);

} // namespace apitab::cli
