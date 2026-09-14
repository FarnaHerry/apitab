// platform/macos/main.cpp — macOS 平台入口（HuxerUI CLI 生成格式）。
// 会话偏好（主题/关闭行为/上次会话）必须在 RunApplication 之前加载。
// argv[1] == "--cli" 时走无 GUI 命令行模式（src/cli.cpp），不进事件循环；
// GUI 路径先获取用户级单实例锁，重复启动直接退出。
#include <huxerui/app.h>

#include <string_view>
#include <vector>

#include "cli.h"
#include "single_instance.h"

import apitab.preferences;

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--cli") {
        loadSessionPreferences();
        return apitab::cli::run(std::vector<std::string>(argv + 2, argv + argc));
    }
    const apitab::SingleInstance single_instance("dev.farna.apitab");
    if (single_instance.alreadyRunning()) {
        return 0;
    }
    if (!single_instance.acquired()) {
        return 1;
    }
    loadSessionPreferences();
    return huxerui::RunApplication();
}
