// platform/windows/main.cpp — Windows 平台入口（HuxerUI CLI 生成格式）。
// 会话偏好（主题/关闭行为/上次会话）必须在 RunApplication 之前加载。
// 链接为 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup（见顶层 CMakeLists），保留 main。
// argv[1] == "--cli" 时把命令转发给运行中的实例（控制面薄客户端，
// 见 docs/plans/runtime-control-surface.md），本进程不进事件循环、不碰数据库；
// GUI 路径先获取命名 Mutex，重复启动直接退出。
#include <huxerui/app.h>

#include <string_view>
#include <vector>

#include "control.h"
#include "single_instance.h"

#include <cstdio>

import apitab.preferences;
import apitab.store.loadtest;
import apitab.store.requests;

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--cli") {
        // 薄客户端：命令发给运行中的实例执行（见 src/control_client.cpp）。
        // 本进程不构造 store、不碰数据库——DB 的唯一属主是 GUI 进程。
        std::vector<std::string> args(argv + 2, argv + argc);
        // --ensure：实例没在跑就先拉起再执行（客户端侧开关，不发给服务端）。
        bool ensure = false;
        if (!args.empty() && args.front() == "--ensure") {
            ensure = true;
            args.erase(args.begin());
        }
        std::string error;
        const int code = apitab::control::ForwardCommand(args, error, ensure);
        if (!error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        return code;
    }
    const apitab::SingleInstance single_instance("dev.farna.apitab");
    if (single_instance.alreadyRunning()) {
        return 0;
    }
    if (!single_instance.acquired()) {
        return 1;
    }
    loadSessionPreferences();
    // 领域 store 只在有 GUI（=有 runtime）的进程里打开：库与 curl 工作线程只属于这个
    // 进程，`--cli` 客户端不打开（见 src/store/requests.cppm 的 RequestStore::Open）。
    g_requests.Open();
    g_loadtest.Open();
    return huxerui::RunApplication();
}
