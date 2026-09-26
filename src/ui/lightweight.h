// lightweight.h — 轻量模式（伪纯 CLI 的进程形态）。
//
// 语义（docs/plans/runtime-control-surface.md §7）：进程留在后台，窗口隐藏（停止出帧，
// 实测 CPU → 0），并释放**应用侧可再生缓存**；控制面继续服务，agent 照常下命令。
//
// 必须在应用线程调用（控制面命令经 ApplicationPoster 投递到应用线程后执行）。
#pragma once

#include <string>

namespace apitab::ui {

struct LightweightResult {
    bool ok = false;
    std::string message;  // 给用户/agent 看的一行结果（stdout）
};

// 进入：隐藏窗口 + 释放语法高亮 provider 缓存与压测输出缓冲。幂等。
LightweightResult EnterLightweightMode();

// 退出：恢复窗口显示（缓存按需重建）。幂等。
LightweightResult ExitLightweightMode();

bool LightweightActive();

} // namespace apitab::ui
