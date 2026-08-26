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

    // 组织栏优先 150~220，但极窄窗口必须给内容区留出至少 160 —— 组织栏收缩，
    // 不再用 min 150 把内容区压成负宽。
    const float orgListW = std::min(std::clamp(w * 0.24f, 150.0f, 220.0f),
                                    nonNegative(w - kIslandGap - 160.0f));
    const float gap = kIslandGap;
    const float contentX = x + orgListW + gap + kPanelPad;
    const float contentW = nonNegative(w - orgListW - gap - kPanelPad * 2.0f);

    drawIslandPanel(ui, "home.orgs.island", x, y, orgListW, h, theme,
                    theme.dark ? 0.56f : 0.78f);
    drawIslandPanel(ui, "home.projects.island", contentX, y, contentW, h, theme,
                    theme.dark ? 0.62f : 0.84f);

    ui.text("home.orgs.title")
        .position(x, y).size(nonNegative(orgListW - 104.0f), 24.0f).text("组织")
        .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    if (orgListW >= 100.0f) {
        components::button(ui, "home.org.add")
            .position(x + orgListW - 96.0f, y).size(96.0f, 24.0f)
            .icon(0xF067).text("新建团队").fontSize(kFontLabel).theme(tokens, false)
            .radius(kButtonRadius)
            .onClick([] {
                g_newOrgName.clear();
                g_newOrgOpen = true;
            }).build();
    }

    const float orgScrollH = nonNegative(h - 34.0f - kPanelPad);
    if (orgListW > 0.0f && orgScrollH > 0.0f) {
        components::scrollView(ui, "home.orgs.scroll")
            .position(x + kPanelPad, y + 34.0f).size(orgListW, orgScrollH).theme(tokens)
            .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
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
    }

    const db::Org* org = selectedOrg();
    std::vector<db::Project> projects;
    for (const auto& project : all) {
        if (project.orgId == g_homeSelectedOrgId) projects.push_back(project);
    }

    const float tabY = y + kPanelPad;
    const float tabH = 28.0f;
    // 三个标签优先 88 宽；内容区不够时等比收缩，始终留在 contentW 内。
    const float tabW = std::min(88.0f, nonNegative(contentW - 8.0f) / 3.0f);
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
        const float createW = std::min(108.0f, contentW);
        ui.text("home.projects.title")
            .position(contentX, y + 38.0f).size(nonNegative(contentW - createW - gap), 24.0f)
            .text(org ? org->name + " · 项目" : "项目")
            .fontSize(kFontBody + 2.0f).lineHeight(24.0f).color(theme.titleText)
            .verticalAlign(core::VerticalAlign::Center).build();
        if (createW > 0.0f) {
            components::button(ui, "home.project.add")
                .position(contentX + nonNegative(contentW - createW), y + 38.0f).size(createW, 24.0f)
                .icon(0xF067).text("新建项目").fontSize(kFontLabel).theme(tokens, false)
                .radius(kButtonRadius)
                .onClick([] {
                    g_newProjectName.clear();
                    g_newProjectOpen = true;
                }).build();
        }

        // 卡片网格是独立垂直 scroll：标题/新建按钮固定，只有网格滚动。
        // 列数按可用宽阈值 3/2/1，单列时卡片宽 = 内容宽（不再 min 150 硬顶）。
        const float cardGap = 12.0f;
        const float minCardW = 150.0f;
        const float cardH = 120.0f;
        const float cardTop = y + 72.0f;
        const float gridH = nonNegative(h - 72.0f - kPanelPad);
        const float menuScreenW = screen.width;
        const float menuScreenH = screen.height;
        if (contentW > 0.0f && gridH > 0.0f) {
            components::scrollView(ui, "home.projects.scroll")
                .position(contentX, cardTop).size(contentW, gridH).theme(tokens)
                .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
                .content([&](eui::Ui& cu, float gridW, float) {
                    const float gw = nonNegative(gridW - 4.0f);
                    int cols = 1;
                    if (gw >= 3.0f * minCardW + 2.0f * cardGap) cols = 3;
                    else if (gw >= 2.0f * minCardW + cardGap) cols = 2;
                    const float cardW = nonNegative((gw - cardGap * static_cast<float>(cols - 1))
                                                    / static_cast<float>(cols));
                    const int rows = static_cast<int>((projects.size() + cols - 1) / cols);
                    for (int row = 0; row < rows; ++row) {
                        const std::string rowId = "home.project.row." + std::to_string(row);
                        // scrollView content 是 column 弹性容器：每行用 stack 占位，
                        // 卡片在行内绝对定位；行高包含卡片间距，scroll 高度为真实总行高。
                        cu.stack(rowId)
                            .size(gw, cardH + cardGap)
                            .content([&] {
                                for (int col = 0; col < cols; ++col) {
                                    const std::size_t i = static_cast<std::size_t>(row * cols + col);
                                    if (i >= projects.size()) break;
                                    const db::Project& project = projects[i];
                                    const std::string cardId = "home.project." + std::to_string(project.id);
                                    const float cx = static_cast<float>(col) * (cardW + cardGap);
                                    cu.stack(cardId).position(cx, 0).size(cardW, cardH).content([&] {
                                        cu.rect(cardId + ".bg")
                                            .size(cardW, cardH)
                                            .states(components::theme::withAlpha(tokens.surface, 0.6f),
                                                    tokens.surfaceHover, tokens.surfaceActive)
                                            .radius(kPanelRadius)
                                            .border(1.0f, components::theme::withAlpha(tokens.border, 0.5f))
                                            .onClick([orgIdValue = project.orgId, projectId = project.id] {
                                                openProjectWorkspace(orgIdValue, projectId);
                                            }).build();
                                        cu.text(cardId + ".name")
                                            .position(12.0f, 12.0f).size(nonNegative(cardW - 50.0f), 22.0f)
                                            .text(project.name).fontSize(kFontBody + 1.0f).color(theme.titleText)
                                            .verticalAlign(core::VerticalAlign::Center).build();
                                        cu.text(cardId + ".meta")
                                            .position(12.0f, 44.0f).size(nonNegative(cardW - 24.0f), 18.0f)
                                            .text("点击卡片进入项目工作区").fontSize(kFontLabel).color(theme.metaText).build();
                                        const std::string menuId = cardId + ".menu";
                                        const float menuX = nonNegative(cardW - kCardActionSize - 8.0f);
                                        components::button(cu, menuId)
                                            .position(menuX, 8.0f).size(kCardActionSize, kCardActionSize)
                                            .icon(0xF141).text("").iconSize(kCardActionIconSize).theme(tokens, false)
                                            .radius(kCardActionSize * 0.5f).build();
                                        components::mouseArea(cu, menuId + ".hit")
                                            .position(menuX, 8.0f).size(kCardActionSize, kCardActionSize).zIndex(2)
                                            .onTap([projectId = project.id, menuScreenW, menuScreenH](const components::MouseEvent& event) {
                                                g_projectMenuTargetId = projectId;
                                                // 菜单 150 宽 / 两行高，锚点 clamp 在屏幕内
                                                g_projectMenuX = std::clamp(event.bounds.x, 0.0f,
                                                                            nonNegative(menuScreenW - 154.0f));
                                                g_projectMenuY = std::clamp(event.bounds.y + event.bounds.height,
                                                                            0.0f, nonNegative(menuScreenH - 64.0f));
                                                g_projectMenuOpen = true;
                                            }).build();
                                    }).build();
                                }
                            })
                            .build();
                    }
                }).build();
        }
    } else if (g_homeTab == HomeTab::Members) {
        const float branchH = nonNegative(h - 34.0f);
        drawIslandPanel(ui, "home.members.island", contentX, y + 34.0f,
                        contentW, branchH, theme,
                        theme.dark ? 0.56f : 0.78f);
        if (branchH >= 120.0f && contentW >= 40.0f) {
            ui.text("home.members.title")
                .position(contentX + kPanelPad, y + 42.0f).size(nonNegative(contentW - kPanelPad * 2.0f), 26.0f).text("成员动态")
                .fontSize(kFontBody + 2.0f).color(theme.titleText).build();
            ui.rect("home.member.card")
                .position(contentX + kPanelPad, y + 80.0f)
                .size(std::min(520.0f, nonNegative(contentW - kPanelPad * 2.0f)), 74.0f)
                .color(components::theme::withAlpha(tokens.surface, 0.6f)).radius(kPanelRadius).build();
            ui.text("home.member.current")
                .position(contentX + kPanelPad + 16.0f, y + 94.0f)
                .size(std::min(480.0f, nonNegative(contentW - kPanelPad * 2.0f - 32.0f)), 22.0f)
                .text("当前用户").fontSize(kFontBody).color(theme.bodyText).build();
            ui.text("home.member.hint")
                .position(contentX + kPanelPad + 16.0f, y + 122.0f)
                .size(std::min(480.0f, nonNegative(contentW - kPanelPad * 2.0f - 32.0f)), 20.0f)
                .text("当前仅支持单用户，成员管理功能待后续开放")
                .fontSize(kFontLabel).color(theme.hintText).build();
        }
    } else {
        const std::int64_t selectedOrgId = org ? org->id : 0;
        const std::string selectedOrgName = org ? org->name : std::string{};
        const float branchH = nonNegative(h - 34.0f);
        drawIslandPanel(ui, "home.org.settings.island", contentX, y + 34.0f,
                        contentW, branchH, theme,
                        theme.dark ? 0.56f : 0.78f);
        if (branchH >= 160.0f && contentW >= 120.0f) {
        const float innerX = contentX + kPanelPad;
        const float innerW = nonNegative(contentW - kPanelPad * 2.0f);
        ui.text("home.org.settings.title")
            .position(innerX, y + 42.0f).size(innerW, 26.0f).text("组织设置")
            .fontSize(kFontBody + 2.0f).color(theme.titleText).build();
        ui.text("home.org.settings.name")
            .position(innerX, y + 84.0f).size(90.0f, kInputHeight).text("组织名称")
            .fontSize(kFontLabel).lineHeight(kInputHeight).color(theme.metaText).build();
        ui.text("home.org.settings.value")
            .position(innerX + 100.0f, y + 84.0f).size(nonNegative(innerW - 100.0f), kInputHeight)
            .text(selectedOrgName).fontSize(kFontBody).lineHeight(kInputHeight).color(theme.bodyText).build();
        components::button(ui, "home.org.settings.rename")
            .position(innerX, y + 126.0f).size(96.0f, 26.0f)
            .text("重命名").fontSize(kFontLabel).theme(tokens, false)
            .radius(kButtonRadius)
            .onClick([selectedOrgId, selectedOrgName] {
                if (selectedOrgId == 0) return;
                g_homeOrgRenameText = selectedOrgName;
                g_homeOrgRenameOpen = true;
            }).build();
        if (innerW >= 220.0f) {
            components::button(ui, "home.org.settings.dissolve")
                .position(innerX + 108.0f, y + 126.0f).size(96.0f, 26.0f)
                .text("解散组织").fontSize(kFontLabel).theme(tokens, false)
                .radius(kButtonRadius)
                .onClick([selectedOrgId, selectedOrgName, all] {
                    if (selectedOrgId == 0) return;
                    std::vector<db::Project> projects;
                    for (const auto& project : all) if (project.orgId == selectedOrgId) projects.push_back(project);
                    removeOrg(selectedOrgId, selectedOrgName, projects);
                }).build();
        }
        }
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
        drawInputDialog(ui, screen, theme, "home.project.rename.dialog", "重命名项目",
                        g_homeProjectRenameText, "项目名称", "保存",
                        [] { g_projectRenameOpen = false; g_homeProjectRenameId = 0; },
                        [] {
                            const std::string name = trim(g_homeProjectRenameText);
                            if (name.empty()) { showStatus("项目名称不能为空"); return; }
                            const std::string err = g_requests.renameProject(g_homeProjectRenameId, name);
                            showStatus(err.empty() ? "项目名称已更新" : ("重命名失败: " + err));
                            if (err.empty()) { g_projectRenameOpen = false; g_homeProjectRenameId = 0; }
                        });
    }

    if (g_homeOrgRenameOpen) {
        const std::int64_t orgId = g_homeSelectedOrgId;
        drawInputDialog(ui, screen, theme, "home.org.rename.dialog", "重命名组织",
                        g_homeOrgRenameText, "组织名称", "保存",
                        [] { g_homeOrgRenameOpen = false; },
                        [orgId] {
                            const std::string name = trim(g_homeOrgRenameText);
                            if (name.empty()) { showStatus("组织名称不能为空"); return; }
                            const std::string err = g_requests.renameOrg(orgId, name);
                            showStatus(err.empty() ? "组织名称已更新" : ("重命名失败: " + err));
                            if (err.empty()) g_homeOrgRenameOpen = false;
                        });
    }

    if (g_newOrgOpen) {
        drawInputDialog(ui, screen, theme, "home.org.new.dialog", "新建团队",
                        g_newOrgName, "团队名称", "创建",
                        [] { g_newOrgOpen = false; g_newOrgName.clear(); },
                        [] {
                            const std::string name = trim(g_newOrgName);
                            if (name.empty()) { showStatus("团队名称不能为空"); return; }
                            const std::string err = g_requests.createOrg(name);
                            if (!err.empty()) { showStatus("新建团队失败: " + err); return; }
                            g_newOrgOpen = false;
                            g_newOrgName.clear();
                        });
    }

    if (g_newProjectOpen) {
        const std::int64_t orgId = g_homeSelectedOrgId;
        drawInputDialog(ui, screen, theme, "home.project.new.dialog", "新建项目",
                        g_newProjectName, "项目名称", "创建",
                        [] { g_newProjectOpen = false; g_newProjectName.clear(); },
                        [orgId] {
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
                        });
    }
}
