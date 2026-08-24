// ui/topbars.cppm — 顶部两级栏：组织选择器 + 项目标签页（第 1 条），
// 请求标签页（第 2 条）。层级：组织 → 项目 → 多个请求标签页。
module;

#include "eui_ui.h"

export module apitab.ui.topbars;

import std;
import apitab.db;
import apitab.store.requests;  // g_requests
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

// ---- 项目 / 组织切换（标签页随项目暂存+恢复）----

void switchProject(std::int64_t id) {
    if (id == g_requests.currentProjectId()) return;
    stashTabs(g_requests.currentProjectId());
    if (const std::string err = g_requests.selectProject(id); !err.empty()) {
        showStatus("切换项目失败: " + err);
    }
    restoreTabs(id);
}

void switchOrg(std::int64_t id) {
    if (id == g_requests.currentOrgId()) return;
    stashTabs(g_requests.currentProjectId());
    if (const std::string err = g_requests.selectOrg(id); !err.empty()) {
        showStatus("切换组织失败: " + err);
    }
    restoreTabs(g_requests.currentProjectId());
}

void deleteProjectFlow(std::int64_t id, const std::string& name) {
    askConfirm("删除项目", std::format("将删除项目「{}」及其全部请求，不可恢复。", name),
               [id] {
                   const bool wasCurrent = (id == g_requests.currentProjectId());
                   discardStash(id);
                   if (const std::string err = g_requests.deleteProject(id); !err.empty()) {
                       showStatus("删除项目失败: " + err);
                       return;
                   }
                   if (wasCurrent) clearTabs();
                   restoreTabs(g_requests.currentProjectId());
                   showStatus("项目已删除");
               });
}

void deleteOrgFlow(std::int64_t id, const std::string& name) {
    askConfirm("删除组织",
               std::format("将删除组织「{}」、其全部项目与请求，不可恢复。", name),
               [id] {
                   // 先记下将被级联删除的项目，清理其 tab 暂存。
                   std::vector<std::int64_t> doomed;
                   for (const auto& p : g_requests.projects()) doomed.push_back(p.id);
                   const bool wasCurrent = (id == g_requests.currentOrgId());
                   if (const std::string err = g_requests.deleteOrg(id); !err.empty()) {
                       showStatus("删除组织失败: " + err);
                       return;
                   }
                   for (const std::int64_t pid : doomed) discardStash(pid);
                   if (wasCurrent) clearTabs();
                   restoreTabs(g_requests.currentProjectId());
                   showStatus("组织已删除");
               });
}

// ---- 小图标按钮（顶栏操作：新建/重命名/删除）----

void drawIconBtn(eui::Ui& ui, const std::string& id, float x, float y, unsigned int icon,
                 const AppTheme& theme, std::function<void()> onClick) {
    components::button(ui, id)
        .position(x, y)
        .size(22.0f, 22.0f)
        .icon(icon)
        .text("")
        .iconSize(10.0f)
        .theme(theme.components, false)
        .onClick(std::move(onClick))
        .build();
}

} // namespace

// ---- 第 1 条：组织选择器 + 项目标签页 ----

export void drawOrgProjectBar(eui::Ui& ui, float x, float y, float w,
                              const AppTheme& theme) {
    const auto& tokens = theme.components;
    const auto& orgs = g_requests.orgs();
    const auto& projects = g_requests.projects();

    // 当前组织名与下标
    int orgIndex = 0;
    std::string orgName = "(无组织)";
    std::vector<std::string> orgNames;
    for (int i = 0; i < static_cast<int>(orgs.size()); ++i) {
        orgNames.push_back(orgs[i].name);
        if (orgs[i].id == g_requests.currentOrgId()) {
            orgIndex = i;
            orgName = orgs[i].name;
        }
    }

    // ---- 组织：下拉 或 重命名输入框 ----
    const float orgW = 150.0f;
    static bool orgOpen = false;
    if (g_renamingOrg) {
        components::input(ui, "topbar.org.rename")
            .position(x, y)
            .size(orgW, kInputHeight)
            .value(g_orgRenameText)
            .placeholder("组织名称")
            .theme(tokens)
            .onChange([](const std::string& v) { g_orgRenameText = v; })
            .onEnter([oid = g_requests.currentOrgId()] {
                if (!trim(g_orgRenameText).empty()) {
                    if (const std::string err = g_requests.renameOrg(oid, trim(g_orgRenameText));
                        !err.empty()) {
                        showStatus("重命名失败: " + err);
                    }
                }
                g_renamingOrg = false;
            })
            .onFocus([](bool focused) {
                if (!focused) g_renamingOrg = false;  // 失焦放弃
            })
            .build();
    } else {
        ui.stack("topbar.org.wrap")
            .position(x, y)
            .size(orgW, kInputHeight)
            .zIndex(30)  // 弹层压项目标签与内容
            .content([&] {
                components::dropdown(ui, "topbar.org")
                    .size(orgW, kInputHeight)
                    .items(orgNames)
                    .selected(orgIndex)
                    .open(orgOpen)
                    .theme(tokens)
                    .onOpenChange([](bool o) { orgOpen = o; })
                    .onChange([orgs](int i) { switchOrg(orgs[i].id); })
                    .build();
            })
            .build();
    }

    // 组织操作：新建 / 重命名 / 删除
    float bx = x + orgW + 4.0f;
    drawIconBtn(ui, "topbar.org.add", bx, y + 2.0f, 0xF067, theme, [] {
        if (const std::string err = g_requests.createOrg(
                std::format("组织 {}", g_requests.orgs().size() + 1));
            !err.empty()) {
            showStatus("新建组织失败: " + err);
        }
    });
    bx += 26.0f;
    drawIconBtn(ui, "topbar.org.edit", bx, y + 2.0f, 0xF303, theme, [orgName] {  // fa-pen
        g_orgRenameText = orgName;
        g_renamingOrg = true;
    });
    bx += 26.0f;
    drawIconBtn(ui, "topbar.org.del", bx, y + 2.0f, 0xF1F8, theme,
                [oid = g_requests.currentOrgId(), orgName] { deleteOrgFlow(oid, orgName); });
    bx += 30.0f;

    // ---- 项目标签页（弹性宽度：均分剩余，90..170）----
    const float addW = 26.0f;
    const float avail = std::max(60.0f, x + w - addW - bx);
    const int count = static_cast<int>(projects.size());
    const float tabW = count > 0 ? std::clamp(avail / count, 90.0f, 170.0f) : 170.0f;

    float tx = bx;
    for (const auto& p : projects) {
        const std::string tabId = "topbar.proj." + std::to_string(p.id);
        const bool active = (p.id == g_requests.currentProjectId());
        const bool renaming = (g_renamingProjectId == p.id);

        if (renaming) {
            // 重命名：tab 变输入框，Enter 提交 / 失焦放弃。
            components::input(ui, tabId + ".rename")
                .position(tx, y)
                .size(tabW - 4.0f, kInputHeight)
                .value(g_projectRenameText)
                .placeholder("项目名称")
                .theme(tokens)
                .onChange([](const std::string& v) { g_projectRenameText = v; })
                .onEnter([pid = p.id] {
                    if (!trim(g_projectRenameText).empty()) {
                        if (const std::string err =
                                g_requests.renameProject(pid, trim(g_projectRenameText));
                            !err.empty()) {
                            showStatus("重命名失败: " + err);
                        }
                    }
                    g_renamingProjectId = 0;
                })
                .onFocus([](bool focused) {
                    if (!focused) g_renamingProjectId = 0;
                })
                .build();
        } else {
            ui.stack(tabId)
                .position(tx, y)
                .size(tabW - 4.0f, kInputHeight)
                .content([&] {
                    const float w2 = tabW - 4.0f;
                    // 命中区（点击切换；点已激活 tab 进入重命名）
                    ui.rect(tabId + ".hit")
                        .position(0, 0)
                        .size(w2, kInputHeight)
                        .states(active ? components::theme::withAlpha(tokens.primary, 0.20f)
                                       : components::theme::withAlpha(tokens.surface, 0.5f),
                                tokens.surfaceHover, tokens.surfaceActive)
                        .radius(6.0f)
                        .onClick([pid = p.id, pname = p.name, active] {
                            if (active) {
                                g_projectRenameText = pname;
                                g_renamingProjectId = pid;
                            } else {
                                switchProject(pid);
                            }
                        })
                        .build();
                    ui.text(tabId + ".name")
                        .position(8.0f, 0)
                        .size(w2 - (active ? 32.0f : 12.0f), kInputHeight)
                        .text(p.name)
                        .fontSize(kFontBody)
                        .lineHeight(kInputHeight)
                        .color(active ? theme.titleText : theme.metaText)
                        .verticalAlign(core::VerticalAlign::Center)
                        .build();
                    // 删除（仅激活 tab 显示，避免误点）
                    if (active) {
                        components::button(ui, tabId + ".del")
                            .position(w2 - 24.0f, 2.0f)
                            .size(20.0f, 20.0f)
                            .icon(0xF00D)
                            .text("")
                            .iconSize(8.0f)
                            .theme(tokens, false)
                            .onClick([pid = p.id, pname = p.name] {
                                deleteProjectFlow(pid, pname);
                            })
                            .build();
                    }
                })
                .build();
        }
        tx += tabW;
    }

    // 新建项目
    drawIconBtn(ui, "topbar.proj.add", tx, y + 2.0f, 0xF067, theme, [] {
        if (const std::string err = g_requests.createProject(
                std::format("项目 {}", g_requests.projects().size() + 1));
            !err.empty()) {
            showStatus("新建项目失败: " + err);
        }
    });
}

// ---- 第 2 条：请求标签页 ----

export void drawRequestTabStrip(eui::Ui& ui, float x, float y, float w,
                                const AppTheme& theme) {
    const auto& tokens = theme.components;

    const float addW = 26.0f;
    const float avail = std::max(60.0f, w - addW);
    const int count = static_cast<int>(g_tabs.size());
    const float tabW = count > 0 ? std::clamp(avail / count, 110.0f, 200.0f) : 200.0f;

    float tx = x;
    for (const auto& t : g_tabs) {
        const std::string tabId = "reqtabs.tab." + std::to_string(t.uid);
        const bool active = (t.uid == g_activeTabUid);

        ui.stack(tabId)
            .position(tx, y)
            .size(tabW - 3.0f, 24.0f)
            .content([&] {
                const float w2 = tabW - 3.0f;
                ui.rect(tabId + ".hit")
                    .position(0, 0)
                    .size(w2, 24.0f)
                    .states(active ? components::theme::withAlpha(tokens.primary, 0.20f)
                                   : components::theme::withAlpha(tokens.surface, 0.4f),
                            tokens.surfaceHover, tokens.surfaceActive)
                    .radius(6.0f)
                    .onClick([uid = t.uid] { g_activeTabUid = uid; })
                    .build();
                // 方法徽章
                ui.text(tabId + ".method")
                    .position(6.0f, 0)
                    .size(34.0f, 24.0f)
                    .text(kMethods[t.draft.methodIndex])
                    .fontSize(8.0f)
                    .lineHeight(24.0f)
                    .color(methodColor(kMethods[t.draft.methodIndex], theme))
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
                // 名称（未保存草稿带 •）
                ui.text(tabId + ".name")
                    .position(42.0f, 0)
                    .size(w2 - 42.0f - 24.0f, 24.0f)
                    .text(t.draft.name + (t.requestId == 0 ? " •" : ""))
                    .fontSize(kFontLabel)
                    .lineHeight(24.0f)
                    .color(active ? theme.titleText : theme.metaText)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
                // 关闭
                components::button(ui, tabId + ".close")
                    .position(w2 - 22.0f, 3.0f)
                    .size(18.0f, 18.0f)
                    .icon(0xF00D)
                    .text("")
                    .iconSize(8.0f)
                    .theme(tokens, false)
                    .onClick([uid = t.uid] { closeTab(uid); })
                    .build();
            })
            .build();
        tx += tabW;
    }

    // 新建草稿 tab
    drawIconBtn(ui, "reqtabs.add", tx, y + 1.0f, 0xF067, theme, [] { (void)newDraftTab(); });
}
