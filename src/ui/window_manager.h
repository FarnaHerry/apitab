// window_manager.h — apitab 多窗口控制器（应用级服务）。
//
// 职责：登记主/子窗口句柄、保管跨窗口迁移的标签信封、承载「主窗口桥」——
// 子窗口通过它驱动主窗口的顶级标签模型（唯一合法的跨窗口状态通道：应用级
// 服务 + 主窗口组合期注册的回调；State 句柄始终只被其所属窗口的回调写）。
//
// 线程约定：全部方法只在应用线程调用（组合、事件回调、控制面任务回投后）。
// 主窗口是初始窗口、永不退役，其桥的回调在应用运行期始终有效；子窗口的
// 句柄经 UnregisterChild 在退役清理时移除，不悬挂。
#pragma once

#include <huxerui/huxerui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "draft.h"
#include "ui.h"

namespace apitab::ui {

// 请求工作区快照：拆窗/拖回时随项目标签迁移的窗口内组合态（均为纯值）。
struct WorkspaceSnapshot {
    std::vector<RequestDraft> drafts; // 请求页未保存草稿（含内容、类型、用例等）
    std::size_t active_draft = 0;     // 请求页当前内部标签
    std::size_t nav_page = 0;         // 项目工作区内部页（pages::kRequest 等）
};

// 跨窗口转移信封：发起方写入、接收方按项目 id 消费（一次性）。
struct TabTransferEnvelope {
    std::int64_t project_id = 0;
    WorkspaceSnapshot workspace;
};

// 主窗口桥：主窗口 AppRoot 挂载时注册（SetMainBridge），组合内回调直接写主
// 窗口的顶级标签 State 并持久化 open_projects。子窗口只经 WindowManager 调用。
struct MainBridge {
    // 拆出：主窗口移除该项目标签（状态回退同关闭标签，但不写领域游标、
    // 不清 active_project——子窗口随后会把游标同步到拆出的项目）。
    std::function<void(std::int64_t)> detach_tab;
    // 拖回：主窗口插回该项目标签并激活（含领域同步与持久化）。
    std::function<void(std::int64_t)> restore_tab;
    // 子窗口点主页/齿轮：唤醒主窗口并切换顶级标签（主页或全局设置）。
    std::function<void(TopTabId)> wake_to_tab;
    // 拆出时读取主窗口当前项目工作区内部页（随信封迁移到子窗口）。
    std::function<std::size_t()> current_nav_page;
};

class WindowManager {
public:
    // ---- 窗口登记（各窗口根组合挂载时写入、卸载清理时移除） ----
    void SetMainWindow(huxerui::WindowHandle window) { main_window_.emplace(window); }
    void ClearMainWindow() { main_window_.reset(); }
    void RegisterChild(std::int64_t project_id, huxerui::WindowHandle window,
                       std::uint32_t window_ordinal) {
        children_.insert_or_assign(project_id, ChildEntry{window, window_ordinal});
    }
    void UnregisterChild(std::int64_t project_id) { children_.erase(project_id); }
    bool HasChild(std::int64_t project_id) const { return children_.contains(project_id); }

    // 子窗口根组合注册的工作区内部页提供者（实时拖回的信封要带走 nav_page）。
    void RegisterNavPage(std::uint32_t window_ordinal, std::function<std::size_t()> provider) {
        nav_providers_.insert_or_assign(window_ordinal, std::move(provider));
    }
    void UnregisterNavPage(std::uint32_t window_ordinal) { nav_providers_.erase(window_ordinal); }

    // ---- 主窗口桥 ----
    void SetMainBridge(MainBridge bridge) { main_bridge_ = std::move(bridge); }
    void ClearMainBridge() { main_bridge_.reset(); }

    // ---- 工作区快照提供者（键 = 所在窗口的 WindowIdentity::ordinal） ----
    // RequestPage 挂载时注册、卸载时注销；快照在拆出/拖回的发起窗口侧采集。
    using Capture = std::function<WorkspaceSnapshot()>;
    void RegisterCapture(std::uint32_t window_ordinal, Capture capture) {
        captures_.insert_or_assign(window_ordinal, std::move(capture));
    }
    void UnregisterCapture(std::uint32_t window_ordinal) { captures_.erase(window_ordinal); }

    // ---- 行为 ----

    // 拆出（主窗口项目标签拖出标签条）：采集快照 → 主窗口 detach_tab → 光标处
    // 打开子窗口（key = "project:<id>"，外观参数与主窗口一致）。若该项目已有
    // 子窗口则忽略（同一项目同一时刻只在一个窗口打开）。
    void TearOff(std::uint32_t from_ordinal, std::int64_t project_id,
                 const std::optional<huxerui::Point>& cursor_screen);

    // 拖回（子窗口项目标签拖出窗口顶缘）：采集快照 → 存信封（nav_page 由子窗口
    // 根组合随调用给出）→ 主窗口 restore_tab → 退役子窗口并唤醒主窗口。
    void MergeBack(std::uint32_t from_ordinal, std::int64_t project_id, std::size_t nav_page);

    // ---- 实时拖回（应用级类型化拖拽，全程不松手，Chrome 式）----
    // 原生跨窗口拖拽传输要求源窗口在拖拽期间存活，所以合并拆成三段：
    // MergeBegin（DropTarget::Entered，指针进入主窗口标签条）：采集信封并把
    //   标签插入主窗口（挂起态）；子窗口不退役，仍显示原标签。
    //   @return 是否真的开启了挂起合并（调用方据此记录本地 pending 门控）。
    // MergeAbort（Exited，指针离开标签条）：撤销挂起合并——主窗口移除标签，
    //   信封丢弃；标签与草稿仍在子窗口（其组合态从未动过）。
    // MergeCommit（Dropped，松手确认）：退役子窗口，合并完成，主窗口已挂载的
    //   项目页消费信封恢复草稿。
    bool MergeBegin(std::int64_t project_id);
    void MergeAbort(std::int64_t project_id);
    void MergeCommit(std::int64_t project_id);

    // 关闭子窗口（✕ 或子窗口内点标签 ✕）：等价主窗口里关闭该标签——主窗口
    // detach 后按关闭语义回退并持久化，然后退役子窗口（草稿按设计丢弃）。
    void CloseChildTab(std::int64_t project_id);

    // 子窗口点主页：单纯唤醒主窗口并切主页，子窗口自己不导航。
    void WakeMainToHome() const;

    // 子窗口点齿轮：唤醒主窗口并打开/激活全局设置。
    void WakeMainToSettings() const;

    // 唤醒主窗口（Activate）并经主窗口桥切换顶级标签；无桥时仅激活。
    void WakeMainToTab(TopTabId target) const;

    // 安装期写入：拆出子窗口要经 ApplicationHandle::OpenWindow（组合外可用）。
    void SetApplication(huxerui::ApplicationHandle application) { application_.emplace(application); }

    std::optional<huxerui::WindowHandle> MainWindow() const { return main_window_; }

    // 接收方读取信封（子窗口根组合消费 nav_page、RequestPage 首组合消费草稿；
    // project_id 不匹配时不动信封）。
    std::optional<std::size_t> ConsumeNavPage(std::int64_t project_id);
    std::optional<WorkspaceSnapshot> ConsumeWorkspace(std::int64_t project_id);

private:
    std::optional<WorkspaceSnapshot> CaptureFrom(std::uint32_t window_ordinal) const;

    struct ChildEntry {
        huxerui::WindowHandle handle;
        std::uint32_t ordinal = 0;
    };

    std::optional<huxerui::ApplicationHandle> application_;
    std::optional<huxerui::WindowHandle> main_window_;
    std::unordered_map<std::int64_t, ChildEntry> children_;
    std::optional<MainBridge> main_bridge_;
    std::unordered_map<std::uint32_t, Capture> captures_;
    std::unordered_map<std::uint32_t, std::function<std::size_t()>> nav_providers_;
    std::optional<TabTransferEnvelope> envelope_;
    std::optional<std::int64_t> pending_merge_;
};

// 子窗口 key 约定（WindowOptions::key ↔ WindowIdentity::key）。
inline constexpr char kChildWindowKeyPrefix[] = "project:";
std::int64_t ParseChildWindowKey(const std::string& key);
std::string MakeChildWindowKey(std::int64_t project_id);

// 应用安装钩子（AppOptions::application_hooks）：创建并发布进程级 WindowManager，
// 同时 Provide 给组合（UseService<WindowManager>()）。
void InstallWindowManager(huxerui::ApplicationContext& context);

// 进程级访问器（控制面/轻量模式等非组合上下文使用；组合内请用 UseService）。
WindowManager* CurrentWindowManager();

// 拆出子窗口的窗口选项（外观与主窗口一致；position 由调用方按光标给）。
huxerui::WindowOptions MakeChildWindowOptions(std::int64_t project_id, const std::string& title,
                                              const std::optional<huxerui::Point>& position);

} // namespace apitab::ui
