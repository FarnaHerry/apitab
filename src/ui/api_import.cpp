// api_import.cpp — 接口文件解析实现（桩，待完整实现替换）。
#include "api_import.h"

#include <string>

namespace apitab::ui {

bool ParseApiFile(const std::string& text, ImportedApi& out, std::string& error) {
    (void)text;
    (void)out;
    error = "导入解析尚未实现";
    return false;
}

} // namespace apitab::ui
