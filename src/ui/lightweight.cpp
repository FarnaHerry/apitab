// lightweight.cpp — 轻量模式实现（契约见 lightweight.h）。
//
// 这里是"伪纯 CLI"的进程形态：全部窗口已销毁（Retire，fork 级联退役 owned
// 窗口，渲染内存随窗口由框架释放）、应用侧缓存已释放，但 runtime / 数据库 /
// 控制面 / 托盘都还在，`apitab --cli …` 照常可用；托盘激活或 `--cli show`
// 走 TrayWindowController::ShowWindow 重建主窗口（Clash-Flux 托盘运行时同款
// Retire/重建语义）。
//
// 历史口径：早期实现只 Hide 窗口，实测 224.7 MB 可见 → 224.5 MB 隐藏（Hide
// 几乎不省内存，GL 与 huxerui 渲染缓存仍由 runtime 持有）。fork 收编窗口
// 释放入口（501e8ca：release windows in lightweight tray mode）后升级为
// Retire；本文件的缓存释放保留——销毁窗口不回收控制面持有的应用侧缓存。
#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"
#include "app.h"
#include "lightweight.h"
#include "window_manager.h"

import apitab.store.loadtest;

namespace apitab::ui {
namespace {

bool g_lightweight = false;

} // namespace

bool LightweightActive() { return g_lightweight; }

LightweightResult EnterLightweightMode() {
    if (g_lightweight) {
        return {.ok = true, .message = "已处于轻量模式"};
    }
    TrayWindowController* controller = CurrentWindowController();
    if (controller == nullptr || !controller->HasWindow()) {
        return {.ok = false, .message = "轻量模式失败：窗口尚未挂载（实例可能仍在启动）"};
    }
    // Retire 会级联退役全部窗口（含拆出的项目窗口），未保存草稿随窗口丢弃：
    // 有子窗口或请求页有未保存草稿时拒绝进入，避免静默数据损失。
    if (WindowManager* manager = CurrentWindowManager()) {
        if (manager->HasChildren()) {
            return {.ok = false, .message = "轻量模式失败：请先拖回或关闭拆出的项目窗口"};
        }
        if (manager->AnyUnsavedDrafts()) {
            return {.ok = false, .message = "轻量模式失败：请求页有未保存草稿，请先保存或清理"};
        }
    }

    // 1) 退役全部窗口 → 渲染内存由框架随窗口销毁释放（停止出帧）。
    controller->Retire();
    // 2) 释放应用侧可再生缓存：语法高亮 provider（各自持有解析文档与行索引）
    //    与压测输出缓冲。两者都在下次需要时按需重建。
    ClearSweetLineProviders();
    (void)g_loadtest.drainOutput();
    g_lightweight = true;
    return {.ok = true,
            .message = "已进入轻量模式：窗口已销毁（渲染内存释放），控制面与托盘继续服务"};
}

LightweightResult ExitLightweightMode() {
    if (!g_lightweight) {
        return {.ok = true, .message = "当前不在轻量模式"};
    }
    TrayWindowController* controller = CurrentWindowController();
    if (controller == nullptr) {
        return {.ok = false, .message = "退出轻量模式失败：应用控制器不可用"};
    }
    // 无活窗口 → ShowWindow 走 OpenWindow 重建路径；窗口已可见时幂等激活。
    controller->ShowWindow();
    g_lightweight = false;
    return {.ok = true,
            .message = "已退出轻量模式：主窗口已重建（界面状态按会话恢复，缓存按需重建）"};
}

} // namespace apitab::ui
