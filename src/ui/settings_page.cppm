// ui/settings_page.cppm — 全局设置与当前项目设置页面。
module;

#include "eui_ui.h"

export module apitab.ui.settings_page;

import std;
import apitab.db;
import apitab.i18n;
import apitab.store.requests;
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

bool g_themeModeOpen = false;
bool g_languageOpen = false;
std::int64_t g_projectDraftId = 0;
std::string g_projectNameDraft;

const db::Project* currentProject() {
    const std::int64_t id = g_requests.currentProjectId();
    for (const auto& project : g_requests.projects()) {
        if (project.id == id) return &project;
    }
    return nullptr;
}

std::string currentOrgName(std::int64_t orgId) {
    for (const auto& org : g_requests.orgs()) {
        if (org.id == orgId) return org.name;
    }
    return "未知组织";
}

} // namespace

export void drawGlobalSettingsPage(eui::Ui& ui, float x, float y, float w, float h,
                                   const AppTheme& theme) {
    const auto& tokens = theme.components;
    // 岛屿精确铺满页面，内容统一内缩 kPanelPad 并进 scroll root：
    // 矮窗口滚动到达，不再把固定 y 的控件画出岛外。
    drawIsland(ui, "settings.island", x, y, w, h, theme,
               theme.dark ? 0.66f : 0.84f, kIslandPopupZIndex, [&] {
    const float innerW = nonNegative(w - 2.0f * kPanelPad);
    const float innerH = nonNegative(h - 2.0f * kPanelPad);
    if (innerW <= 0.0f || innerH <= 0.0f) return;
    const bool compact = innerW < 390.0f;
    const float inset = 16.0f;
    // compact：label 在上 field 在下（真正的列布局）；宽窗口 label 左 field 右。
    const float panelH = compact ? 196.0f : 150.0f;
    const float themeLabelY = 66.0f;
    const float themeFieldY = compact ? 92.0f : 66.0f;
    const float languageLabelY = compact ? 132.0f : 104.0f;
    const float languageFieldY = compact ? 158.0f : 104.0f;
    const float hintY = compact ? 194.0f : 142.0f;
    const float bodyH = hintY + 34.0f + 12.0f;
    components::scrollView(ui, "settings.scroll")
        .position(kPanelPad, kPanelPad).size(innerW, innerH).zIndex(20).theme(tokens)
        .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
            const float bodyW = nonNegative(contentWidth);
            cu.stack("settings.body").size(bodyW, std::max(bodyH, viewportH)).content([&] {
                const float fieldW = compact
                    ? nonNegative(bodyW - inset * 2.0f)
                    : nonNegative(bodyW - 134.0f);
                const float fieldX = compact ? inset : 118.0f;
                cu.text("settings.title")
                    .position(0, 12.0f).size(bodyW, 26.0f).text(tr(UiText::GlobalSettings))
                    .fontSize(kFontBody + 2.0f).lineHeight(26.0f).color(theme.titleText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                cu.rect("settings.panel")
                    .position(0, 48.0f).size(bodyW, panelH)
                    .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.56f : 0.72f))
                    .radius(kPanelRadius).build();
                // 注意：不能用 "settings.theme.label" —— dropdown("settings.theme") 的
                // 内部文本就是这个 id，撞名后我们的标签被覆盖掉（id 全局唯一）。
                cu.text("settings.theme.caption")
                    .position(inset, themeLabelY)
                    .size(compact ? fieldW : 96.0f, kInputHeight)
                    .text(tr(UiText::Theme)).fontSize(kFontBody).lineHeight(kInputHeight).color(theme.bodyText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                cu.stack("settings.theme.wrap")
                    .position(fieldX, themeFieldY).size(fieldW, kInputHeight).zIndex(30)
                    .content([&] {
                        registerSelectionPopup("settings.theme", g_themeModeOpen,
                                                [] { g_themeModeOpen = false; });
                        components::dropdown(cu, "settings.theme")
                            .size(fieldW, kInputHeight)
                            .items({tr(UiText::Dark), tr(UiText::Light), tr(UiText::System)})
                            .selected(static_cast<int>(g_themeMode)).open(g_themeModeOpen).theme(tokens)
                            .onOpenChange([](bool open) {
                                g_themeModeOpen = open;
                                setSelectionPopupOpen("settings.theme", open);
                            })
                            .onChange([](int selected) {
                                g_themeMode = static_cast<ThemeMode>(std::clamp(selected, 0, 2));
                                g_savedThemeMode = static_cast<int>(g_themeMode);
                                if (g_themeMode == ThemeMode::System) g_dark = systemDark();
                                saveLanguagePreference();
                                g_themeModeOpen = false;
                            }).build();
                    }).build();
                cu.text("settings.language.caption")
                    .position(inset, languageLabelY)
                    .size(compact ? fieldW : 96.0f, kInputHeight)
                    .text(tr(UiText::Language)).fontSize(kFontBody).lineHeight(kInputHeight).color(theme.bodyText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                cu.stack("settings.language.wrap")
                    .position(fieldX, languageFieldY).size(fieldW, kInputHeight).zIndex(30)
                    .content([&] {
                        registerSelectionPopup("settings.language", g_languageOpen,
                                                [] { g_languageOpen = false; });
                        components::dropdown(cu, "settings.language")
                            .size(fieldW, kInputHeight).items(languageNames())
                            .selected(g_language == Language::English ? 1 : 0).open(g_languageOpen).theme(tokens)
                            .onOpenChange([](bool open) {
                                g_languageOpen = open;
                                setSelectionPopupOpen("settings.language", open);
                            })
                            .onChange([](int selected) {
                                setLanguage(selected == 1 ? Language::English : Language::Chinese);
                                g_languageOpen = false;
                            }).build();
                    }).build();
                cu.text("settings.theme.hint")
                    .position(inset, hintY).size(nonNegative(bodyW - inset * 2.0f), 34.0f)
                    .text(tr(UiText::ThemeHint)).fontSize(kFontLabel).color(theme.hintText).wrap(true).build();
            }).build();
        }).build();
    });
}

export void drawProjectSettingsPage(eui::Ui& ui, float x, float y, float w, float h,
                                    const AppTheme& theme) {
    const auto& tokens = theme.components;
    const db::Project* project = currentProject();
    if (!project) {
        g_page = Page::Home;
        return;
    }
    if (g_projectDraftId != project->id) {
        g_projectDraftId = project->id;
        g_projectNameDraft = project->name;
    }

    // 项目 shell 已由 app.cpp 绘制；页面只提供内容和滚动，不再嵌套第二张岛屿卡片。
    const float innerW = nonNegative(w - 2.0f * kPanelPad);
    const float innerH = nonNegative(h - 2.0f * kPanelPad);
    if (innerW <= 0.0f || innerH <= 0.0f) return;
    const bool compact = innerW < 300.0f;
    const float bodyH = compact ? 240.0f : 176.0f;
    const std::int64_t projectId = project->id;
    const std::string orgName = currentOrgName(project->orgId);
    components::scrollView(ui, "project.settings.scroll")
        .position(x + kPanelPad, y + kPanelPad).size(innerW, innerH).theme(tokens)
        .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
            const float bodyW = nonNegative(contentWidth);
            cu.stack("project.settings.body").size(bodyW, std::max(bodyH, viewportH)).content([&] {
                const float fieldX = compact ? 0.0f : 112.0f;
                const float fieldW = compact ? bodyW : nonNegative(bodyW - 112.0f);
                cu.text("project.settings.title")
                    .position(0, 2.0f).size(bodyW, 26.0f).text("项目设置")
                    .fontSize(kFontBody + 2.0f).lineHeight(26.0f).color(theme.titleText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                // 所属组织（只读）
                cu.text("project.settings.org.label")
                    .position(0, compact ? 44.0f : 52.0f).size(compact ? bodyW : 88.0f, 24.0f)
                    .text("所属组织").fontSize(kFontLabel).lineHeight(24.0f).color(theme.metaText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                cu.text("project.settings.org.value")
                    .position(fieldX, compact ? 70.0f : 52.0f).size(fieldW, 24.0f)
                    .text(orgName).fontSize(kFontBody).lineHeight(24.0f)
                    .color(theme.bodyText).verticalAlign(core::VerticalAlign::Center).build();
                // 项目名称
                cu.text("project.settings.name.label")
                    .position(0, compact ? 108.0f : 84.0f).size(compact ? bodyW : 88.0f, kInputHeight)
                    .text("项目名称").fontSize(kFontLabel).lineHeight(kInputHeight).color(theme.metaText)
                    .verticalAlign(core::VerticalAlign::Center).build();
                components::input(cu, "project.settings.name")
                    .position(fieldX, compact ? 134.0f : 84.0f).size(fieldW, kInputHeight)
                    .value(g_projectNameDraft).theme(tokens)
                    .onChange([](const std::string& value) { g_projectNameDraft = value; })
                    .onEnter([projectId] {
                        const std::string name = trim(g_projectNameDraft);
                        if (name.empty()) { showStatus("项目名称不能为空"); return; }
                        const std::string err = g_requests.renameProject(projectId, name);
                        showStatus(err.empty() ? "项目设置已保存" : ("保存失败: " + err));
                    })
                    .build();
                // 操作行：环境管理在左、保存在右（紧凑模式下仍一行，宽度已验证 ≤ 196）
                const float btnY = compact ? 174.0f : 126.0f;
                components::button(cu, "project.settings.environments")
                    .position(0, btnY).size(102.0f, 24.0f)
                    .icon(0xF013).text("环境管理").fontSize(kFontLabel).iconSize(8.0f)
                    .theme(tokens, false)
                    .radius(kButtonRadius)
                    .onClick([] { g_envManageOpen = true; })
                    .build();
                if (bodyW >= 190.0f) {
                    components::button(cu, "project.settings.save")
                        .position(bodyW - 74.0f, btnY).size(74.0f, 24.0f)
                        .text("保存").fontSize(kFontLabel).theme(tokens, true)
                        .radius(kButtonRadius)
                        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                        .radius(kButtonRadius)
                        .onClick([projectId] {
                            const std::string name = trim(g_projectNameDraft);
                            if (name.empty()) { showStatus("项目名称不能为空"); return; }
                            const std::string err = g_requests.renameProject(projectId, name);
                            showStatus(err.empty() ? "项目设置已保存" : ("保存失败: " + err));
                        })
                        .build();
                }
            }).build();
        }).build();
}
