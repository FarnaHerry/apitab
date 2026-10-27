// window_manager.cpp — 多窗口控制器实现：安装钩子、拆出/拖回/关闭/唤醒行为。
#include "window_manager.h"

#include <utility>

#include "app.h"

import apitab.db;
import apitab.preferences;
import apitab.store.loadtest;
import apitab.store.requests;

namespace apitab::ui {

namespace {

WindowManager* g_windowManager = nullptr;

// 拆出子窗口的默认尺寸与主窗口一致（src/app.cpp 的 AppOptions::window）。
constexpr huxerui::Size kChildInitialSize{1180.0F, 760.0F};
constexpr huxerui::Size kChildMinimumSize{800.0F, 600.0F};

} // namespace

WindowManager* CurrentWindowManager() { return g_windowManager; }

void InstallWindowManager(huxerui::ApplicationContext& context) {
    auto manager = std::make_shared<WindowManager>();
    manager->SetApplication(huxerui::UseApplication());
    g_windowManager = manager.get();
    context.Provide(manager);
}

huxerui::WindowOptions MakeChildWindowOptions(std::int64_t project_id, const std::string& title,
                                              const std::optional<huxerui::Point>& position) {
    huxerui::WindowOptions options;
    options.title = title;
    options.initial_size = kChildInitialSize;
    options.minimum_size = kChildMinimumSize;
    options.chrome_mode = huxerui::WindowChromeMode::Custom;
    options.title_bar_height = kTitleBarContentHeight;
    if (position.has_value()) {
        options.initial_position = position;
    }
    return options;
}

std::optional<WorkspaceSnapshot> WindowManager::CaptureFrom(WindowId window_id) const {
    const auto it = captures_.find(window_id);
    if (it == captures_.end() || !it->second) return std::nullopt;
    return it->second();
}

std::optional<std::size_t> WindowManager::ConsumeNavPage(std::int64_t project_id) {
    if (!envelope_ || envelope_->project_id != project_id) return std::nullopt;
    return envelope_->workspace.nav_page;
}

std::optional<WorkspaceSnapshot> WindowManager::ConsumeWorkspace(std::int64_t project_id) {
    if (!envelope_ || envelope_->project_id != project_id) return std::nullopt;
    WorkspaceSnapshot snapshot = std::move(envelope_->workspace);
    envelope_.reset();
    return snapshot;
}

void WindowManager::TearOff(WindowId from_window_id, std::int64_t project_id,
                            const std::optional<huxerui::Point>& cursor_screen) {
    // 已有同名子窗口（重复触发/竞态）：忽略本次手势，标签留在主窗口。
    if (!main_bridge_ || !main_bridge_->detach_tab || HasChild(project_id)) return;

    // 领域游标先切到拆出项目：子窗口首帧组合（OpenWindow 内同步开始）要读到
    // 正确的项目上下文；主窗口随后的 detach 不写游标（见 MainBridge 契约）。
    if (auto selected = g_requests.selectProject(project_id); !selected) return;
    (void)g_loadtest.setProject(project_id);
    saveSessionPreference("active_project", std::to_string(project_id));

    // 快照在 detach 之前采集：主窗口的项目页此时仍然挂载（IndexedPages 保活）。
    // nav_page 来自主窗口根组合（request 页的快照提供者只管草稿）。
    WorkspaceSnapshot workspace = CaptureFrom(from_window_id).value_or(WorkspaceSnapshot{});
    if (main_bridge_->current_nav_page) workspace.nav_page = main_bridge_->current_nav_page();
    envelope_ = TabTransferEnvelope{project_id, std::move(workspace)};
    main_bridge_->detach_tab(project_id);

    std::string title = "apitab";
    if (const auto projects = g_requests.allProjects()) {
        for (const db::Project& project : *projects) {
            if (project.id == project_id) {
                title = std::string{"apitab — "} + project.name;
                break;
            }
        }
    }
    const huxerui::WindowOptions options = MakeChildWindowOptions(project_id, title, cursor_screen);
    if (application_) {
        static_cast<void>(application_->OpenWindow(
            [project_id] { return ChildWindowRoot(project_id); }, options));
    }
}

void WindowManager::MergeBack(WindowId from_window_id, std::int64_t project_id,
                              std::size_t nav_page) {
    const auto child = children_.find(project_id);
    if (child == children_.end() || !main_bridge_ || !main_bridge_->restore_tab) return;

    WorkspaceSnapshot workspace = CaptureFrom(from_window_id).value_or(WorkspaceSnapshot{});
    workspace.nav_page = nav_page;
    envelope_ = TabTransferEnvelope{project_id, std::move(workspace)};
    main_bridge_->restore_tab(project_id);
    child->second.handle.Retire();
    if (main_window_) main_window_->Activate();
}

bool WindowManager::MergeBegin(std::int64_t project_id) {
    // 已挂起（重复 Entered）：幂等。 子窗口必须存在（只有拆出窗口的标签参与
    // 实时拖回）；主窗口桥负责把标签插入主窗口（含领域同步与持久化）。
    if (pending_merge_ == project_id) return true;
    if (pending_merge_.has_value() || !main_bridge_ || !main_bridge_->restore_tab) return false;
    const auto child = children_.find(project_id);
    if (child == children_.end()) return false;
    // 信封从子窗口的快照提供者采集（其请求页仍挂载、状态活跃），nav_page 由
    // 子窗口根组合的提供者给出；MergeAbort 即整份丢弃。
    WorkspaceSnapshot workspace = CaptureFrom(child->second.window_id).value_or(WorkspaceSnapshot{});
    if (const auto nav = nav_providers_.find(child->second.window_id);
        nav != nav_providers_.end() && nav->second) {
        workspace.nav_page = nav->second();
    }
    envelope_ = TabTransferEnvelope{project_id, std::move(workspace)};
    main_bridge_->restore_tab(project_id);
    pending_merge_ = project_id;
    return true;
}

void WindowManager::MergeAbort(std::int64_t project_id) {
    if (pending_merge_ != project_id) return;
    pending_merge_.reset();
    envelope_.reset();
    if (main_bridge_ && main_bridge_->detach_tab) {
        main_bridge_->detach_tab(project_id);
    }
}

void WindowManager::MergeCommit(std::int64_t project_id) {
    if (pending_merge_ != project_id) return;
    pending_merge_.reset();
    const auto child = children_.find(project_id);
    if (child != children_.end()) {
        child->second.handle.Retire();
    }
    if (main_window_) main_window_->Activate();
}

void WindowManager::CloseChildTab(std::int64_t project_id) {
    const auto child = children_.find(project_id);
    if (child == children_.end() || !main_bridge_ || !main_bridge_->detach_tab) return;
    // 主窗口按关闭语义回退（含持久化）；领域游标若在被关项目上，桥实现负责
    // 清零（与主窗口内 ✕ 关标签一致）。草稿按产品决策随窗口丢弃。
    main_bridge_->detach_tab(project_id);
    child->second.handle.Retire();
    if (main_window_) main_window_->Activate();
}

void WindowManager::WakeMainToHome() const { WakeMainToTab(TopTabId{}); }

void WindowManager::WakeMainToSettings() const {
    WakeMainToTab(TopTabId{TopTabKind::GlobalSettings, 0});
}

void WindowManager::WakeMainToTab(TopTabId target) const {
    if (main_window_) main_window_->Activate();
    if (main_bridge_ && main_bridge_->wake_to_tab) {
        main_bridge_->wake_to_tab(target);
        return;
    }
    // 主窗口桥缺失（轻量模式或主窗口退役窗口期）：唤回重建主窗口；目标标签
    // 的切换交由新主窗口按会话持久化恢复，不在本次调用里补投。
    if (TrayWindowController* controller = CurrentWindowController()) {
        controller->ShowWindow();
    }
}

} // namespace apitab::ui
