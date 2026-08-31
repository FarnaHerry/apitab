// cli.h — apitab 命令行模式（`apitab --cli <子命令>`，无 GUI）。
// 实现见 src/cli.cpp。main() 在解析到 --cli 时调用 cli::run 并以其返回值退出。
#pragma once

#include <string>
#include <vector>

namespace apitab::cli {

int run(const std::vector<std::string>& args); // 返回进程退出码

} // namespace apitab::cli
