// platform/macos/main.cpp — macOS 平台入口（HuxerUI CLI 生成格式）。
// 会话偏好（主题/关闭行为/上次会话）必须在 RunApplication 之前加载。
#include <huxerui/app.h>

import apitab.preferences;

int main() {
    loadSessionPreferences();
    return huxerui::RunApplication();
}
