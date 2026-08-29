// platform/windows/main.cpp — Windows 平台入口（HuxerUI CLI 生成格式）。
// 会话偏好（主题/关闭行为/上次会话）必须在 RunApplication 之前加载。
// 链接为 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup（见顶层 CMakeLists），保留 main。
#include <huxerui/app.h>

import apitab.preferences;

int main() {
    loadSessionPreferences();
    return huxerui::RunApplication();
}
