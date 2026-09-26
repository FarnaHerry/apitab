// control.h — 运行中实例的控制面（`apitab --cli` 的服务端/客户端共同契约）。
//
// 形态见 docs/plans/runtime-control-surface.md：DB 与 store 的唯一属主是 **GUI 进程**，
// `apitab --cli …` 只是运行中实例的薄客户端——命令在实例的**应用线程**上执行，
// 结果原样回传。
//
// 端点：127.0.0.1 上的一次性端口 + 每次启动重新生成的随机 token；两者写进
// `$XDG_RUNTIME_DIR/apitab-control.json`（POSIX，0600；Windows 回落数据目录），
// 进程退出即删除。只绑回环、只认带 token 的请求。
#pragma once

#include <huxerui/app.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace apitab::control {

// 端点文件路径（缺失目录会被创建）。
std::string EndpointFilePath();

// 读取端点文件里的 {port, token}；文件缺失/损坏 → false，error 给出可读原因。
bool ReadEndpoint(int& port, std::string& token, std::string& error);

// 把一条命令转发给运行中的实例：命令在实例的应用线程执行，stdout/stderr 原样回放
// 到本进程标准流，返回它的退出码。连接失败时返回 1 并填充 error（调用方负责打印）。
int ForwardCommand(const std::vector<std::string>& args, std::string& error);

// 应用线程投递口：控制面线程 → 应用线程的唯一通道。
// 应用安装期还没有组合作用域（拿不到 TaskScope::Post），所以由根组合在挂载时注入
// `TaskScope::Post`（见 src/ui/app.cpp），卸载时清空。线程安全：Set/Clear 来自
// 应用线程，Post 来自控制面线程。
class ApplicationPoster {
public:
    using Post = std::function<void(std::function<void()>)>;

    void Set(Post post);
    void Clear();
    // 投递一条命令；尚未注入（或已随组合卸载）→ false，调用方回 503。
    bool PostTask(std::function<void()> task) const;

private:
    mutable std::mutex mutex_;
    Post post_;
};

// 应用安装钩子（AppOptions::application_hooks）：起控制面服务。
// 起不来只打印一行 stderr，不拖垮 GUI——agent 侧会看到"实例未运行/端点不可达"。
void InstallControlServer(huxerui::ApplicationContext& context);

} // namespace apitab::control
