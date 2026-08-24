// ui/home_page.cppm — 主页面：左侧组织列表，右侧项目卡片（横向排列）。
module;

#include "eui_ui.h"

export module apitab.ui.home_page;

import std;
import apitab.db;
import apitab.store.requests;
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.topbars;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

std::string g_newOrgName;
std::string g_newProjectName;
std::int64_t g_homeRenamingOrgId = 0;
std::string g_homeOrgRenameText;
std::int64_t g_homeRenamingProjectId = 0;
std::string g_homeProjectRenameText;

void removeProject(std::int64_t id, const std::string& name) {
    askConfirm("删除项目", std::format("将删除项目「{}」及其全部请求，不可恢复。", name), [id] {
        const std::string err = g_requests.deleteProject(id);
        if (!err.empty()) {
            showStatus("删除项目失败: " + err);
            return;
        }
        forgetProjectWorkspace(id);
        showStatus("项目已删除");
    });
}

void removeOrg(std::int64_t id, const std::string& name, const std::vector<db::Project>& projects) {
    askConfirm("删除组织", std::format("将删除组织「{}」及其全部项目与请求，不可恢复。", name),
               [id, projects] {
                   const std::string err = g_requests.deleteOrg(id);
                   if (!err.empty()) {
                       showStatus("删除组织失败: " + err);
                       return;
                   }
                   for (const auto& project : projects) forgetProjectWorkspace(project.id);
                   showStatus("组织已删除");
               });
}

} // namespace

export void drawHomePage(eui::Ui& ui, float x, float y, float w, float h,
                         const AppTheme& theme) {
    const auto& tokens = theme.components;
    const auto& orgs = g_requests.orgs();
    const std::vector<db::Project> all = g_requests.allProjects();

    // 选中组织失效（被删/首启）→ 选第一个。
    const bool selectedValid = std::ranges::any_of(orgs, [&](const db::Org& o) {
        return o.id == g_homeSelectedOrgId;
    });
    if (!selectedValid && !orgs.empty()) g_homeSelectedOrgId = orgs.front().id;

    const float orgListW = std::clamp(w * 0.24f, 150.0f, 220.0f);
    const float gap = 12.0f;
    const float contentX = x + orgListW + gap;
    const float contentW = std::max(0.0f, w - orgListW - gap);

    // ---- 左侧：组织列表 ----
    ui.text("home.orgs.title")
        .position(x, y).size(orgListW, 24.0f).text("组织")
        .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    components::input(ui, "home.org.new")
        .position(x, y + 30.0f).size(orgListW - 96.0f, kInputHeight)
        .value(g_newOrgName).placeholder("新组织名称").theme(tokens)
        .onChange([](const std::string& value) { g_newOrgName = value; }).build();
    components::button(ui, "home.org.add")
        .position(x + orgListW - 88.0f, y + 30.0f).size(88.0f, kInputHeight)
        .icon(0xF067).text("新建").fontSize(kFontLabel).theme(tokens, false)
        .onClick([] {
            const std::string name = trim(g_newOrgName);
            if (name.empty()) { showStatus("组织名称不能为空"); return; }
            const std::string err = g_requests.createOrg(name);
            if (!err.empty()) { showStatus("新建组织失败: " + err); return; }
            g_newOrgName.clear();
        }).build();

    components::scrollView(ui, "home.orgs.scroll")
        .position(x, y + 66.0f).size(orgListW, std::max(40.0f, h - 66.0f)).theme(tokens)
        .content([&](eui::Ui& cu, float contentWidth, float) {
            for (const auto& org : orgs) {
                const std::string orgId = "home.org." + std::to_string(org.id);
                const bool selected = org.id == g_homeSelectedOrgId;
                cu.stack(orgId)
                    .size(contentWidth - 4.0f, 30.0f)
                    .content([&] {
                        cu.rect(orgId + ".bg")
                            .size(contentWidth - 4.0f, 30.0f)
                            .states(selected ? components::theme::withAlpha(tokens.primary, 0.18f)
                                            : components::theme::withAlpha(tokens.surface, 0.5f),
                                    tokens.surfaceHover, tokens.surfaceActive)
                            .radius(6.0f)
                            .onClick([id = org.id] { g_homeSelectedOrgId = id; })
                            .build();
                        cu.text(orgId + ".name")
                            .position(10.0f, 0).size(contentWidth - 60.0f, 30.0f)
                            .text(org.name).fontSize(kFontBody).lineHeight(30.0f)
                            .color(selected ? theme.titleText : theme.bodyText)
                            .verticalAlign(core::VerticalAlign::Center).build();
                        components::button(cu, orgId + ".rename")
                            .position(contentWidth - 48.0f, 5.0f).size(20.0f, 20.0f)
                            .icon(0xF303).text("").iconSize(8.0f).theme(tokens, false)
                            .onClick([id = org.id, name = org.name] {
                                g_homeOrgRenameText = name;
                                g_homeRenamingOrgId = id;
                            }).build();
                        components::button(cu, orgId + ".delete")
                            .position(contentWidth - 24.0f, 5.0f).size(20.0f, 20.0f)
                            .icon(0xF1F8).text("").iconSize(8.0f).theme(tokens, false)
                            .onClick([id = org.id, name = org.name, all] {
                                std::vector<db::Project> projects;
                                for (const auto& project : all) if (project.orgId == id) projects.push_back(project);
                                removeOrg(id, name, projects);
                            }).build();
                    })
                    .build();
            }
        }).build();

    // ---- 右侧：选中组织的项目卡片（横向排列）----
    std::vector<db::Project> projects;
    for (const auto& project : all) {
        if (project.orgId == g_homeSelectedOrgId) projects.push_back(project);
    }

    ui.text("home.projects.title")
        .position(contentX, y).size(contentW, 24.0f).text("项目")
        .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();

    const float cardGap = 12.0f;
    const float cardW = std::clamp((contentW - cardGap * 2.0f) / 3.0f, 150.0f, 240.0f);
    const float cardH = 120.0f;
    const float cardTop = y + 34.0f;

    // 新建项目卡片（始终在最前）。
    const std::string newId = "home.project.new";
    ui.stack(newId)
        .position(contentX, cardTop).size(cardW, cardH)
        .content([&] {
            ui.rect(newId + ".bg")
                .size(cardW, cardH)
                .states(components::theme::withAlpha(tokens.surface, 0.5f),
                        tokens.surfaceHover, tokens.surfaceActive)
                .radius(kPanelRadius)
                .border(1.0f, components::theme::withAlpha(tokens.border, 0.5f))
                .build();
            ui.text(newId + ".title")
                .position(12.0f, 12.0f).size(cardW - 24.0f, 20.0f)
                .text("新建项目").fontSize(kFontBody).color(theme.titleText).build();
            components::input(ui, newId + ".name")
                .position(12.0f, 40.0f).size(cardW - 24.0f, kInputHeight)
                .value(g_newProjectName).placeholder("项目名称").theme(tokens)
                .onChange([](const std::string& value) { g_newProjectName = value; }).build();
            components::button(ui, newId + ".add")
                .position(12.0f, 76.0f).size(cardW - 24.0f, 28.0f)
                .icon(0xF067).text("创建").fontSize(kFontLabel).theme(tokens, true)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                .onClick([orgIdValue = g_homeSelectedOrgId] {
                    const std::string name = trim(g_newProjectName);
                    if (name.empty()) { showStatus("项目名称不能为空"); return; }
                    const std::string selectErr = g_requests.selectOrg(orgIdValue);
                    if (!selectErr.empty()) { showStatus("选择组织失败: " + selectErr); return; }
                    const std::string err = g_requests.createProject(name);
                    if (!err.empty()) { showStatus("新建项目失败: " + err); return; }
                    const std::int64_t id = g_requests.currentProjectId();
                    g_newProjectName.clear();
                    openProjectWorkspace(orgIdValue, id);
                }).build();
        })
        .build();

    // 项目卡片。
    for (std::size_t i = 0; i < projects.size(); ++i) {
        const db::Project& project = projects[i];
        const std::string cardId = "home.project." + std::to_string(project.id);
        const float col = static_cast<float>(i % 3);
        const float row = static_cast<float>(i / 3);
        const float cx = contentX + col * (cardW + cardGap);
        const float cy = cardTop + row * (cardH + cardGap);
        ui.stack(cardId)
            .position(cx, cy).size(cardW, cardH)
            .content([&] {
                ui.rect(cardId + ".bg")
                    .size(cardW, cardH)
                    .states(components::theme::withAlpha(tokens.surface, 0.6f),
                            tokens.surfaceHover, tokens.surfaceActive)
                    .radius(kPanelRadius)
                    .border(1.0f, components::theme::withAlpha(tokens.border, 0.5f))
                    .onClick([orgIdValue = project.orgId, projectId = project.id] {
                        openProjectWorkspace(orgIdValue, projectId);
                    })
                    .build();
                ui.text(cardId + ".name")
                    .position(12.0f, 12.0f).size(cardW - 24.0f, 22.0f)
                    .text(project.name).fontSize(kFontBody + 1.0f).color(theme.titleText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                ui.text(cardId + ".meta")
                    .position(12.0f, 40.0f).size(cardW - 24.0f, 18.0f)
                    .text("项目").fontSize(kFontLabel).color(theme.metaText).build();
                components::button(ui, cardId + ".open")
                    .position(12.0f, 76.0f).size(64.0f, 28.0f)
                    .text("打开").fontSize(kFontLabel).theme(tokens, true)
                    .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                    .onClick([orgIdValue = project.orgId, projectId = project.id] {
                        openProjectWorkspace(orgIdValue, projectId);
                    }).build();
                components::button(ui, cardId + ".rename")
                    .position(cardW - 96.0f, 76.0f).size(40.0f, 28.0f)
                    .icon(0xF303).text("").iconSize(9.0f).theme(tokens, false)
                    .onClick([id = project.id, name = project.name] {
                        g_homeProjectRenameText = name;
                        g_homeRenamingProjectId = id;
                    }).build();
                components::button(ui, cardId + ".delete")
                    .position(cardW - 50.0f, 76.0f).size(38.0f, 28.0f)
                    .icon(0xF1F8).text("").iconSize(9.0f).theme(tokens, false)
                    .onClick([id = project.id, name = project.name] { removeProject(id, name); }).build();
            })
            .build();
    }

    // 项目重命名内联输入（覆盖在对应卡片上）。
    if (g_homeRenamingProjectId != 0) {
        for (std::size_t i = 0; i < projects.size(); ++i) {
            const db::Project& project = projects[i];
            if (project.id != g_homeRenamingProjectId) continue;
            const float col = static_cast<float>(i % 3);
            const float row = static_cast<float>(i / 3);
            const float cx = contentX + col * (cardW + cardGap);
            const float cy = cardTop + row * (cardH + cardGap);
            const std::string rid = "home.project.rename." + std::to_string(project.id);
            ui.stack(rid)
                .position(cx, cy).size(cardW, cardH).zIndex(10)
                .content([&] {
                    ui.rect(rid + ".bg")
                        .size(cardW, cardH)
                        .color(components::theme::withAlpha(tokens.surface, 0.95f))
                        .radius(kPanelRadius)
                        .border(1.0f, components::theme::withAlpha(tokens.border, 0.6f))
                        .build();
                    ui.text(rid + ".title")
                        .position(12.0f, 12.0f).size(cardW - 24.0f, 20.0f)
                        .text("重命名项目").fontSize(kFontBody).color(theme.titleText).build();
                    components::input(ui, rid + ".name")
                        .position(12.0f, 40.0f).size(cardW - 24.0f, kInputHeight)
                        .value(g_homeProjectRenameText).theme(tokens)
                        .onChange([](const std::string& value) { g_homeProjectRenameText = value; })
                        .onEnter([id = project.id] {
                            const std::string name = trim(g_homeProjectRenameText);
                            if (!name.empty()) (void)g_requests.renameProject(id, name);
                            g_homeRenamingProjectId = 0;
                        }).build();
                    components::button(ui, rid + ".save")
                        .position(12.0f, 76.0f).size(64.0f, 28.0f)
                        .text("保存").fontSize(kFontLabel).theme(tokens, true)
                        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                        .onClick([id = project.id] {
                            const std::string name = trim(g_homeProjectRenameText);
                            if (!name.empty()) (void)g_requests.renameProject(id, name);
                            g_homeRenamingProjectId = 0;
                        }).build();
                    components::button(ui, rid + ".cancel")
                        .position(cardW - 76.0f, 76.0f).size(64.0f, 28.0f)
                        .text("取消").fontSize(kFontLabel).theme(tokens, false)
                        .onClick([] { g_homeRenamingProjectId = 0; }).build();
                })
                .build();
        }
    }
}
