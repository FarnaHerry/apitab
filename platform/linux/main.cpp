// platform/linux/main.cpp — Linux 平台入口（HuxerUI CLI 生成格式）。
// 会话偏好（主题/关闭行为/上次会话）必须在 RunApplication 之前加载。
// argv[1] == "--cli" 时走无 GUI 命令行模式（src/cli.cpp），不进事件循环；
// GUI 路径零变化。
#include <huxerui/app.h>

#include <string_view>
#include <vector>

#include "cli.h"

import apitab.preferences;

int main(int argc, char** argv) {
    loadSessionPreferences();
    if (argc > 1 && std::string_view(argv[1]) == "--cli") {
        return apitab::cli::run(std::vector<std::string>(argv + 2, argv + argc));
    }
    return huxerui::RunApplication();
}
