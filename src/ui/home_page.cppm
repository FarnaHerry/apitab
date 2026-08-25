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
bool g_newOrgOpen = false;
bool g_newProjectOpen = false;
bool g_projectMenuOpen = false;
std::int64_t g_projectMenuTargetId = 0;
float g_projectMenuX = 0.0f;
float g_projectMenuY = 0.0f;
bool g_projectRenameOpen = false;
std::int64_t g_homeProjectRenameId = 0;
std::string g_homeProjectRenameText;
bool g_homeOrgRenameOpen = false;
std::string g_homeOrgRenameText;

const db::Org* selectedOrg() {
    for (const auto& org : g_requests.orgs()) {
        if (org.id == g_homeSelectedOrgId) return &org;
    }
    return nullptr;
}

void removeProject(std::int64_t id, const std::string& name) {
    askConfirm("删除项目", std::format("将删除项目「{}」及其全部请求，不可恢复。", name), [id] {
        const std::string err = g_requests.deleteProject(id);
        if (!err.empty()) {
            showStatus("删除项目失败: " + err);
            return;
        }
        forgetProjectWorkspace(id);
        g_projectMenuOpen = false;
        showStatus("项目已删除");
    });
}

void removeOrg(std::int64_t id, const std::string& name,
               const std::vector<db::Project>& projects) {
    askConfirm("解散组织", std::format("将解散组织「{}」及其全部项目与请求，不可恢复。", name),
               [id, projects] {
                   const std::string err = g_requests.deleteOrg(id);
                   if (!err.empty()) {
                       showStatus("解散组织失败: " + err);
                       return;
                   }
                   for (const auto& project : projects) forgetProjectWorkspace(project.id);
                   g_homeSelectedOrgId = 0;
                   g_homeTab = HomeTab::Projects;
                   g_page = Page::Home;
                   showStatus("组织已解散");
               });
}

} // namespace

export void drawHomePage(eui::Ui& ui, const eui::Screen& screen, float x, float y, float w, float h,
                         const AppTheme& theme) {
    const auto& tokens = theme.components;
    const auto& orgs = g_requests.orgs();
    const std::vector<db::Project> all = g_requests.allProjects();

    const bool selectedValid = std::ranges::any_of(orgs, [&](const db::Org& o) {
        return o.id == g_homeSelectedOrgId;
    });
    if (!selectedValid && !orgs.empty()) {
        g_homeSelectedOrgId = orgs.front().id;
        g_homeTab = HomeTab::Projects;
    }

    const float orgListW = std::clamp(w * 0.24f, 150.0f, 220.0f);
    const float gap = 12.0f;
    const float contentX = x + orgListW + gap;
    const float contentW = std::max(0.0f, w - orgListW - gap);

    ui.text("home.orgs.title")
        .position(x, y).size(orgListW - 104.0f, 24.0f).text("组织")
        .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    components::button(ui, "home.org.add")
        .position(x + orgListW - 96.0f, y).size(96.0f, 24.0f)
        .icon(0xF067).text("新建团队").fontSize(kFontLabel).theme(tokens, false)
        .onClick([] {
            g_newOrgName.clear();
            g_newOrgOpen = true;
        }).build();

    components::scrollView(ui, "home.orgs.scroll")
        .position(x, y + 34.0f).size(orgListW, std::max(40.0f, h - 34.0f)).theme(tokens)
        .content([&](eui::Ui& cu, float contentWidth, float) {
            for (const auto& org : orgs) {
                const std::string orgId = "home.org." + std::to_string(org.id);
                const bool selected = org.id == g_homeSelectedOrgId;
                cu.stack(orgId)
                    .size(std::max(0.0f, contentWidth - 4.0f), 30.0f)
                    .content([&] {
                        cu.rect(orgId + ".bg")
                            .size(std::max(0.0f, contentWidth - 4.0f), 30.0f)
                            .states(selected ? components::theme::withAlpha(tokens.primary, 0.18f)
                                             : components::theme::withAlpha(tokens.surface, 0.5f),
                                    tokens.surfaceHover, tokens.surfaceActive)
                            .radius(6.0f)
                            .onClick([id = org.id] {
                                if (g_homeSelectedOrgId != id) g_homeTab = HomeTab::Projects;
                                g_homeSelectedOrgId = id;
                                g_projectMenuOpen = false;
                                persistSessionState();
                            })
                            .build();
                        cu.text(orgId + ".name")
                            .position(10.0f, 0).size(std::max(0.0f, contentWidth - 20.0f), 30.0f)
                            .text(org.name).fontSize(kFontBody).lineHeight(30.0f)
                            .color(selected ? theme.titleText : theme.bodyText)
                            .verticalAlign(core::VerticalAlign::Center).build();
                    })
                    .build();
            }
        }).build();

    const db::Org* org = selectedOrg();
    std::vector<db::Project> projects;
    for (const auto& project : all) {
        if (project.orgId == g_homeSelectedOrgId) projects.push_back(project);
    }

    const float tabY = y;
    const float tabH = 28.0f;
    const float tabW = 88.0f;
    const std::array<std::string, 3> tabLabels = {"项目", "成员动态", "组织设置"};
    for (int i = 0; i < 3; ++i) {
        const HomeTab tab = static_cast<HomeTab>(i);
        const std::string id = "home.tab." + std::to_string(i);
        components::button(ui, id)
            .position(contentX + static_cast<float>(i) * (tabW + 4.0f), tabY)
            .size(tabW, tabH).text(tabLabels[static_cast<std::size_t>(i)])
            .fontSize(kFontLabel).theme(tokens, g_homeTab == tab)
            .textColor(g_homeTab == tab ? onPrimaryColor(theme) : tokens.text)
            .iconColor(g_homeTab == tab ? onPrimaryColor(theme) : tokens.text)
            .onClick([tab] {
                g_homeTab = tab;
                g_projectMenuOpen = false;
                persistSessionState();
            }).build();
    }

    if (g_homeTab == HomeTab::Projects) {
        const float createW = 108.0f;
        ui.text("home.projects.title")
            .position(contentX, y + 38.0f).size(std::max(0.0f, contentW - createW - gap), 24.0f)
            .text(org ? org->name + " · 项目" : "项目")
            .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
            .verticalAlign(core::VerticalAlign::Center).build();
        components::button(ui, "home.project.add")
            .position(contentX + std::max(0.0f, contentW - createW), y + 38.0f).size(createW, 24.0f)
            .icon(0xF067).text("新建项目").fontSize(kFontLabel).theme(tokens, false)
            .onClick([] {
                g_newProjectName.clear();
                g_newProjectOpen = true;
            }).build();

        const float cardGap = 12.0f;
        const float cardW = std::clamp((contentW - cardGap * 2.0f) / 3.0f, 150.0f, 240.0f);
        const float cardH = 120.0f;
        const float cardTop = y + 72.0f;
        for (std::size_t i = 0; i < projects.size(); ++i) {
            const db::Project& project = projects[i];
            const std::string cardId = "home.project." + std::to_string(project.id);
            const float col = static_cast<float>(i % 3);
            const float row = static_cast<float>(i / 3);
            const float cx = contentX + col * (cardW + cardGap);
            const float cy = cardTop + row * (cardH + cardGap);
            ui.stack(cardId).position(cx, cy).size(cardW, cardH).content([&] {
                ui.rect(cardId + ".bg")
                    .size(cardW, cardH)
                    .states(components::theme::withAlpha(tokens.surface, 0.6f),
                            tokens.surfaceHover, tokens.surfaceActive)
                    .radius(kPanelRadius)
                    .border(1.0f, components::theme::withAlpha(tokens.border, 0.5f))
                    .onClick([orgIdValue = project.orgId, projectId = project.id] {
                        openProjectWorkspace(orgIdValue, projectId);
                    }).build();
                ui.text(cardId + ".name")
                    .position(12.0f, 12.0f).size(std::max(0.0f, cardW - 50.0f), 22.0f)
                    .text(project.name).fontSize(kFontBody + 1.0f).color(theme.titleText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                ui.text(cardId + ".meta")
                    .position(12.0f, 44.0f).size(std::max(0.0f, cardW - 24.0f), 18.0f)
                    .text("点击卡片进入项目工作区").fontSize(kFontLabel).color(theme.metaText).build();
                const std::string menuId = cardId + ".menu";
                components::button(ui, menuId)
                    .position(cardW - 30.0f, 8.0f).size(22.0f, 22.0f)
                    .icon(0xF141).text("").iconSize(10.0f).theme(tokens, false).build();
                components::mouseArea(ui, menuId + ".hit")
                    .position(cardW - 30.0f, 8.0f).size(22.0f, 22.0f).zIndex(2)
                    .onTap([projectId = project.id](const components::MouseEvent& event) {
                        g_projectMenuTargetId = projectId;
                        g_projectMenuX = event.bounds.x;
                        g_projectMenuY = event.bounds.y + event.bounds.height;
                        g_projectMenuOpen = true;
                    }).build();
            }).build();
        }
    } else if (g_homeTab == HomeTab::Members) {
        ui.text("home.members.title")
            .position(contentX, y + 42.0f).size(contentW, 26.0f).text("成员动态")
            .fontSize(kFontBody + 2.0f).color(theme.titleText).build();
        ui.rect("home.member.card")
            .position(contentX, y + 80.0f).size(std::min(520.0f, contentW), 74.0f)
            .color(components::theme::withAlpha(tokens.surface, 0.6f)).radius(kPanelRadius).build();
        ui.text("home.member.current")
            .position(contentX + 16.0f, y + 94.0f).size(std::min(480.0f, contentW - 32.0f), 22.0f)
            .text("当前用户").fontSize(kFontBody).color(theme.bodyText).build();
        ui.text("home.member.hint")
            .position(contentX + 16.0f, y + 122.0f).size(std::min(480.0f, contentW - 32.0f), 20.0f)
            .text("当前仅支持单用户，成员管理功能待后续开放")
            .fontSize(kFontLabel).color(theme.hintText).build();
    } else {
        const std::int64_t selectedOrgId = org ? org->id : 0;
        const std::string selectedOrgName = org ? org->name : std::string{};
        ui.text("home.org.settings.title")
            .position(contentX, y + 42.0f).size(contentW, 26.0f).text("组织设置")
            .fontSize(kFontBody + 2.0f).color(theme.titleText).build();
        ui.text("home.org.settings.name")
            .position(contentX, y + 84.0f).size(90.0f, kInputHeight).text("组织名称")
            .fontSize(kFontLabel).lineHeight(kInputHeight).color(theme.metaText).build();
        ui.text("home.org.settings.value")
            .position(contentX + 100.0f, y + 84.0f).size(std::max(0.0f, contentW - 100.0f), kInputHeight)
            .text(selectedOrgName).fontSize(kFontBody).lineHeight(kInputHeight).color(theme.bodyText).build();
        components::button(ui, "home.org.settings.rename")
            .position(contentX, y + 126.0f).size(96.0f, 26.0f)
            .text("重命名").fontSize(kFontLabel).theme(tokens, false)
            .onClick([selectedOrgId, selectedOrgName] {
                if (selectedOrgId == 0) return;
                g_homeOrgRenameText = selectedOrgName;
                g_homeOrgRenameOpen = true;
            }).build();
        components::button(ui, "home.org.settings.dissolve")
            .position(contentX + 108.0f, y + 126.0f).size(96.0f, 26.0f)
            .text("解散组织").fontSize(kFontLabel).theme(tokens, false)
            .onClick([selectedOrgId, selectedOrgName, all] {
                if (selectedOrgId == 0) return;
                std::vector<db::Project> projects;
                for (const auto& project : all) if (project.orgId == selectedOrgId) projects.push_back(project);
                removeOrg(selectedOrgId, selectedOrgName, projects);
            }).build();
    }

    if (g_projectMenuOpen) {
        std::vector<components::ContextMenuItem> items{
            {"重命名项目"}, {"删除项目"}
        };
        components::contextMenu(ui, "home.project.context.menu")
            .open(true).screen(screen.width, screen.height)
            .position(g_projectMenuX, g_projectMenuY).size(150.0f, 26.0f)
            .items(std::move(items)).theme(tokens).zIndex(100)
            .onSelect([](int index) {
                const std::int64_t projectId = g_projectMenuTargetId;
                g_projectMenuOpen = false;
                std::string projectName;
                for (const auto& item : g_requests.allProjects()) {
                    if (item.id == projectId) {
                        projectName = item.name;
                        break;
                    }
                }
                if (projectName.empty()) return;
                if (index == 0) {
                    g_homeProjectRenameId = projectId;
                    g_homeProjectRenameText = projectName;
                    g_projectRenameOpen = true;
                } else {
                    removeProject(projectId, projectName);
                }
            }).onOpenChange([](bool open) { g_projectMenuOpen = open; }).build();
    }

    if (g_projectRenameOpen) {
        components::dialog(ui, "home.project.rename.dialog")
            .open(true).screen(screen.width, screen.height).size(360.0f, 154.0f)
            .title("重命名项目").theme(tokens)
            .content([&] {
                components::input(ui, "home.project.rename.input")
                    .position(20.0f, 56.0f).size(320.0f, kInputHeight)
                    .value(g_homeProjectRenameText).placeholder("项目名称").theme(tokens)
                    .onChange([](const std::string& value) { g_homeProjectRenameText = value; }).build();
                components::button(ui, "home.project.rename.cancel")
                    .position(184.0f, 104.0f).size(74.0f, 24.0f)
                    .text("取消").fontSize(kFontLabel).theme(tokens, false)
                    .onClick([] { g_projectRenameOpen = false; g_homeProjectRenameId = 0; }).build();
                components::button(ui, "home.project.rename.confirm")
                    .position(266.0f, 104.0f).size(74.0f, 24.0f)
                    .text("保存").fontSize(kFontLabel).theme(tokens, true)
                    .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                    .onClick([] {
                        const std::string name = trim(g_homeProjectRenameText);
                        if (name.empty()) { showStatus("项目名称不能为空"); return; }
                        const std::string err = g_requests.renameProject(g_homeProjectRenameId, name);
                        showStatus(err.empty() ? "项目名称已更新" : ("重命名失败: " + err));
                        if (err.empty()) { g_projectRenameOpen = false; g_homeProjectRenameId = 0; }
                    }).build();
            }).build();
    }

    if (g_homeOrgRenameOpen) {
        const std::int64_t orgId = g_homeSelectedOrgId;
        components::dialog(ui, "home.org.rename.dialog")
            .open(true).screen(screen.width, screen.height).size(360.0f, 154.0f)
            .title("重命名组织").theme(tokens)
            .content([&] {
                components::input(ui, "home.org.rename.input")
                    .position(20.0f, 56.0f).size(320.0f, kInputHeight)
                    .value(g_homeOrgRenameText).placeholder("组织名称").theme(tokens)
                    .onChange([](const std::string& value) { g_homeOrgRenameText = value; }).build();
                components::button(ui, "home.org.rename.cancel")
                    .position(184.0f, 104.0f).size(74.0f, 24.0f)
                    .text("取消").fontSize(kFontLabel).theme(tokens, false)
                    .onClick([] { g_homeOrgRenameOpen = false; }).build();
                components::button(ui, "home.org.rename.confirm")
                    .position(266.0f, 104.0f).size(74.0f, 24.0f)
                    .text("保存").fontSize(kFontLabel).theme(tokens, true)
                    .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                    .onClick([orgId] {
                        const std::string name = trim(g_homeOrgRenameText);
                        if (name.empty()) { showStatus("组织名称不能为空"); return; }
                        const std::string err = g_requests.renameOrg(orgId, name);
                        showStatus(err.empty() ? "组织名称已更新" : ("重命名失败: " + err));
                        if (err.empty()) g_homeOrgRenameOpen = false;
                    }).build();
            }).build();
    }

    if (g_newOrgOpen) {
        components::dialog(ui, "home.org.new.dialog")
            .open(true).screen(screen.width, screen.height).size(360.0f, 154.0f)
            .title("新建团队").theme(tokens)
            .content([&] {
                components::input(ui, "home.org.new.dialog.input")
                    .position(20.0f, 56.0f).size(320.0f, kInputHeight)
                    .value(g_newOrgName).placeholder("团队名称").theme(tokens)
                    .onChange([](const std::string& value) { g_newOrgName = value; }).build();
                components::button(ui, "home.org.new.dialog.cancel")
                    .position(184.0f, 104.0f).size(74.0f, 24.0f)
                    .text("取消").fontSize(kFontLabel).theme(tokens, false)
                    .onClick([] { g_newOrgOpen = false; g_newOrgName.clear(); }).build();
                components::button(ui, "home.org.new.dialog.confirm")
                    .position(266.0f, 104.0f).size(74.0f, 24.0f)
                    .text("创建").fontSize(kFontLabel).theme(tokens, true)
                    .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                    .onClick([] {
                        const std::string name = trim(g_newOrgName);
                        if (name.empty()) { showStatus("团队名称不能为空"); return; }
                        const std::string err = g_requests.createOrg(name);
                        if (!err.empty()) { showStatus("新建团队失败: " + err); return; }
                        g_newOrgOpen = false;
                        g_newOrgName.clear();
                    }).build();
            }).build();
    }

    if (g_newProjectOpen) {
        const std::int64_t orgId = g_homeSelectedOrgId;
        components::dialog(ui, "home.project.new.dialog")
            .open(true).screen(screen.width, screen.height).size(360.0f, 154.0f)
            .title("新建项目").theme(tokens)
            .content([&] {
                components::input(ui, "home.project.new.dialog.input")
                    .position(20.0f, 56.0f).size(320.0f, kInputHeight)
                    .value(g_newProjectName).placeholder("项目名称").theme(tokens)
                    .onChange([](const std::string& value) { g_newProjectName = value; }).build();
                components::button(ui, "home.project.new.dialog.cancel")
                    .position(184.0f, 104.0f).size(74.0f, 24.0f)
                    .text("取消").fontSize(kFontLabel).theme(tokens, false)
                    .onClick([] { g_newProjectOpen = false; g_newProjectName.clear(); }).build();
                components::button(ui, "home.project.new.dialog.confirm")
                    .position(266.0f, 104.0f).size(74.0f, 24.0f)
                    .text("创建").fontSize(kFontLabel).theme(tokens, true)
                    .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                    .onClick([orgId] {
                        const std::string name = trim(g_newProjectName);
                        if (name.empty()) { showStatus("项目名称不能为空"); return; }
                        const std::string selectErr = g_requests.selectOrg(orgId);
                        if (!selectErr.empty()) { showStatus("选择组织失败: " + selectErr); return; }
                        const std::string err = g_requests.createProject(name);
                        if (!err.empty()) { showStatus("新建项目失败: " + err); return; }
                        const std::int64_t projectId = g_requests.currentProjectId();
                        g_newProjectOpen = false;
                        g_newProjectName.clear();
                        openProjectWorkspace(orgId, projectId);
                    }).build();
            }).build();
    }
}
