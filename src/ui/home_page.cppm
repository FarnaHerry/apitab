// ui/home_page.cppm — 固定主页面：组织和项目管理。
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
std::unordered_map<std::int64_t, std::string> g_newProjectNames;
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
    ui.text("home.title")
        .position(x, y).size(w, 24.0f).text("组织与项目")
        .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();

    components::input(ui, "home.org.new")
        .position(x, y + 32.0f).size(180.0f, kInputHeight)
        .value(g_newOrgName).placeholder("新组织名称").theme(tokens)
        .onChange([](const std::string& value) { g_newOrgName = value; }).build();
    components::button(ui, "home.org.add")
        .position(x + 188.0f, y + 32.0f).size(88.0f, kInputHeight)
        .icon(0xF067).text("新建组织").fontSize(kFontLabel).theme(tokens, false)
        .onClick([] {
            const std::string name = trim(g_newOrgName);
            if (name.empty()) { showStatus("组织名称不能为空"); return; }
            const std::string err = g_requests.createOrg(name);
            if (!err.empty()) { showStatus("新建组织失败: " + err); return; }
            g_newOrgName.clear();
        }).build();

    const auto& orgs = g_requests.orgs();
    const std::vector<db::Project> all = g_requests.allProjects();
    components::scrollView(ui, "home.orgs.scroll")
        .position(x, y + 68.0f).size(w, std::max(40.0f, h - 68.0f)).theme(tokens)
        .content([&](eui::Ui& cu, float contentWidth, float) {
            for (const auto& org : orgs) {
                std::vector<db::Project> projects;
                for (const auto& project : all) if (project.orgId == org.id) projects.push_back(project);
                const std::string orgId = "home.org." + std::to_string(org.id);
                cu.stack(orgId)
                    .size(contentWidth - 4.0f, 30.0f + static_cast<float>(projects.size()) * 28.0f + 32.0f)
                    .content([&] {
                        cu.rect(orgId + ".bg").size(contentWidth - 4.0f,
                            30.0f + static_cast<float>(projects.size()) * 28.0f + 32.0f)
                            .color(components::theme::withAlpha(tokens.surface, 0.6f))
                            .radius(kPanelRadius).build();
                        if (g_homeRenamingOrgId == org.id) {
                            components::input(cu, orgId + ".rename.input")
                                .position(10.0f, 3.0f).size(contentWidth - 150.0f, 24.0f)
                                .value(g_homeOrgRenameText).theme(tokens)
                                .onChange([](const std::string& value) { g_homeOrgRenameText = value; })
                                .onEnter([id = org.id] {
                                    const std::string name = trim(g_homeOrgRenameText);
                                    if (!name.empty()) (void)g_requests.renameOrg(id, name);
                                    g_homeRenamingOrgId = 0;
                                }).build();
                        } else {
                            cu.text(orgId + ".name")
                                .position(10.0f, 0).size(contentWidth - 150.0f, 30.0f)
                                .text(org.name).fontSize(kFontBody).lineHeight(30.0f)
                                .color(theme.titleText).verticalAlign(core::VerticalAlign::Center).build();
                        }
                        components::button(cu, orgId + ".rename")
                            .position(contentWidth - 98.0f, 4.0f).size(20.0f, 20.0f)
                            .icon(0xF303).text("").iconSize(8.0f).theme(tokens, false)
                            .onClick([id = org.id, name = org.name] {
                                g_homeOrgRenameText = name;
                                g_homeRenamingOrgId = id;
                            }).build();
                        components::button(cu, orgId + ".delete")
                            .position(contentWidth - 70.0f, 4.0f).size(20.0f, 20.0f)
                            .icon(0xF1F8).text("").iconSize(8.0f).theme(tokens, false)
                            .onClick([id = org.id, name = org.name, projects] { removeOrg(id, name, projects); }).build();
                        float rowY = 30.0f;
                        for (const auto& project : projects) {
                            if (g_homeRenamingProjectId == project.id) {
                                components::input(cu, orgId + ".project.rename." + std::to_string(project.id))
                                    .position(18.0f, rowY + 2.0f).size(contentWidth - 170.0f, 24.0f)
                                    .value(g_homeProjectRenameText).theme(tokens)
                                    .onChange([](const std::string& value) { g_homeProjectRenameText = value; })
                                    .onEnter([id = project.id] {
                                        const std::string name = trim(g_homeProjectRenameText);
                                        if (!name.empty()) (void)g_requests.renameProject(id, name);
                                        g_homeRenamingProjectId = 0;
                                    }).build();
                            } else {
                                cu.text(orgId + ".project." + std::to_string(project.id))
                                    .position(18.0f, rowY).size(contentWidth - 170.0f, 28.0f)
                                    .text(project.name).fontSize(kFontLabel).lineHeight(28.0f)
                                    .color(theme.bodyText).verticalAlign(core::VerticalAlign::Center).build();
                            }
                            components::button(cu, orgId + ".rename." + std::to_string(project.id))
                                .position(contentWidth - 154.0f, rowY + 3.0f).size(20.0f, 20.0f)
                                .icon(0xF303).text("").iconSize(8.0f).theme(tokens, false)
                                .onClick([id = project.id, name = project.name] {
                                    g_homeProjectRenameText = name;
                                    g_homeRenamingProjectId = id;
                                }).build();
                            components::button(cu, orgId + ".open." + std::to_string(project.id))
                                .position(contentWidth - 126.0f, rowY + 3.0f).size(48.0f, 22.0f)
                                .text("打开").fontSize(kFontLabel).theme(tokens, false)
                                .onClick([orgIdValue = org.id, projectId = project.id] {
                                    openProjectWorkspace(orgIdValue, projectId);
                                }).build();
                            components::button(cu, orgId + ".del." + std::to_string(project.id))
                                .position(contentWidth - 70.0f, rowY + 3.0f).size(20.0f, 20.0f)
                                .icon(0xF1F8).text("").iconSize(8.0f).theme(tokens, false)
                                .onClick([id = project.id, name = project.name] { removeProject(id, name); }).build();
                            rowY += 28.0f;
                        }
                        const std::string& newName = g_newProjectNames[org.id];
                        components::input(cu, orgId + ".new")
                            .position(18.0f, rowY + 3.0f).size(180.0f, 24.0f)
                            .value(newName).placeholder("新项目名称").theme(tokens)
                            .onChange([id = org.id](const std::string& value) { g_newProjectNames[id] = value; }).build();
                        components::button(cu, orgId + ".new.add")
                            .position(206.0f, rowY + 3.0f).size(78.0f, 24.0f)
                            .icon(0xF067).text("新建项目").fontSize(kFontLabel).theme(tokens, false)
                            .onClick([orgIdValue = org.id] {
                                const std::string name = trim(g_newProjectNames[orgIdValue]);
                                if (name.empty()) { showStatus("项目名称不能为空"); return; }
                                const std::string selectErr = g_requests.selectOrg(orgIdValue);
                                if (!selectErr.empty()) { showStatus("选择组织失败: " + selectErr); return; }
                                const std::string err = g_requests.createProject(name);
                                if (!err.empty()) { showStatus("新建项目失败: " + err); return; }
                                const std::int64_t id = g_requests.currentProjectId();
                                g_newProjectNames.erase(orgIdValue);
                                openProjectWorkspace(orgIdValue, id);
                            }).build();
                    }).build();
            }
        }).build();
}
