// lightweight.cpp — 轻量模式实现（契约见 lightweight.h）。
//
// 这里是"伪纯 CLI"的进程形态：窗口不可见、缓存已释放，但 runtime / 数据库 / 控制面 /
// 托盘都还在，`apitab --cli …` 照常可用。
//
// 实测口径（本机 KDE/Wayland，Release 构建）：224.7 MB 可见 → 224.5 MB 仅隐藏
// （Hide 几乎不省内存，GL 与 huxerui 渲染缓存仍由 runtime 持有）；本文件负责的是
// **应用侧那部分可再生缓存**。真正的内存大头（字形图集/纹理/布局缓存）要等上游提供
// 释放入口——见计划 §7.3。
#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"
#include "app.h"
#include "lightweight.h"

import apitab.store.loadtest;

namespace apitab::ui {
namespace {

bool g_lightweight = false;

} // namespace

bool LightweightActive() { return g_lightweight; }

LightweightResult EnterLightweightMode() {
    TrayWindowController* controller = CurrentWindowController();
    if (controller == nullptr || !controller->HasWindow()) {
        return {.ok = false, .message = "轻量模式失败：窗口尚未挂载（实例可能仍在启动）"};
    }
    if (g_lightweight) {
        return {.ok = true, .message = "已处于轻量模式"};
    }

    // 1) 隐藏窗口 → 停止出帧（实测隐藏态 CPU ≈ 0%）。
    controller->Hide();
    // 2) 释放应用侧可再生缓存：语法高亮 provider（各自持有解析文档与行索引）
    //    与压测输出缓冲。两者都在下次需要时按需重建。
    ClearSweetLineProviders();
    (void)g_loadtest.drainOutput();
    g_lightweight = true;
    return {.ok = true,
            .message = "已进入轻量模式：窗口已隐藏，语法高亮与压测输出缓存已释放"};
}

LightweightResult ExitLightweightMode() {
    TrayWindowController* controller = CurrentWindowController();
    if (controller == nullptr || !controller->HasWindow()) {
        return {.ok = false, .message = "退出轻量模式失败：窗口尚未挂载"};
    }
    if (!g_lightweight) {
        return {.ok = true, .message = "当前不在轻量模式"};
    }
    controller->Activate();
    g_lightweight = false;
    return {.ok = true, .message = "已退出轻量模式：窗口已恢复（缓存按需重建）"};
}

} // namespace apitab::ui
