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
    ui.stack("settings.page")
        .position(x, y).size(w, h)
        .content([&] {
            const float contentW = std::min(560.0f, std::max(220.0f, w - 48.0f));
            const float contentX = std::max(24.0f, (w - contentW) * 0.5f);
            ui.rect("settings.page.surface")
                .size(w, h)
                .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.20f : 0.38f))
                .build();
            const bool compact = contentW < 390.0f;
            const float panelH = compact ? 260.0f : 156.0f;
            const float inset = 16.0f;
            const float labelW = compact ? contentW - inset * 2.0f : 96.0f;
            const float fieldX = compact ? contentX + inset : contentX + 118.0f;
            const float fieldW = std::max(120.0f, compact ? contentW - inset * 2.0f : contentW - 134.0f);
            const float themeLabelY = 86.0f;
            const float themeFieldY = compact ? 112.0f : 86.0f;
            const float languageLabelY = compact ? 150.0f : 124.0f;
            const float languageFieldY = compact ? 176.0f : 124.0f;
            const float hintY = compact ? 212.0f : 162.0f;
            drawIslandPanel(ui, "settings.island", contentX, 20.0f, contentW, panelH + 58.0f, theme,
                            theme.dark ? 0.76f : 0.90f);
            ui.text("settings.title")
                .position(contentX, 30.0f).size(contentW, 26.0f).text(tr(UiText::GlobalSettings))
                .fontSize(kFontBody + 2.0f).lineHeight(26.0f).color(theme.titleText)
                .verticalAlign(core::VerticalAlign::Center).build();
            ui.rect("settings.panel")
                .position(contentX, 68.0f).size(contentW, panelH)
                .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.56f : 0.72f))
                .radius(kPanelRadius).build();
            ui.text("settings.theme.label")
                .position(contentX + inset, themeLabelY).size(labelW, kInputHeight)
                .text(tr(UiText::Theme)).fontSize(kFontBody).lineHeight(kInputHeight).color(theme.bodyText)
                .verticalAlign(core::VerticalAlign::Center).build();
            ui.stack("settings.theme.wrap")
                .position(fieldX, themeFieldY).size(fieldW, kInputHeight).zIndex(20)
                .content([&] {
                    components::dropdown(ui, "settings.theme")
                        .size(fieldW, kInputHeight)
                        .items({tr(UiText::Dark), tr(UiText::Light), tr(UiText::System)})
                        .selected(static_cast<int>(g_themeMode)).open(g_themeModeOpen).theme(tokens)
                        .onOpenChange([](bool open) { g_themeModeOpen = open; })
                        .onChange([](int selected) {
                            g_themeMode = static_cast<ThemeMode>(std::clamp(selected, 0, 2));
                            g_savedThemeMode = static_cast<int>(g_themeMode);
                            if (g_themeMode == ThemeMode::System) g_dark = systemDark();
                            saveLanguagePreference();
                            g_themeModeOpen = false;
                        }).build();
                }).build();
            ui.text("settings.language.label")
                .position(contentX + inset, languageLabelY).size(labelW, kInputHeight)
                .text(tr(UiText::Language)).fontSize(kFontBody).lineHeight(kInputHeight).color(theme.bodyText)
                .verticalAlign(core::VerticalAlign::Center).build();
            ui.stack("settings.language.wrap")
                .position(fieldX, languageFieldY).size(fieldW, kInputHeight).zIndex(20)
                .content([&] {
                    components::dropdown(ui, "settings.language")
                        .size(fieldW, kInputHeight).items(languageNames())
                        .selected(g_language == Language::English ? 1 : 0).open(g_languageOpen).theme(tokens)
                        .onOpenChange([](bool open) { g_languageOpen = open; })
                        .onChange([](int selected) {
                            setLanguage(selected == 1 ? Language::English : Language::Chinese);
                            g_languageOpen = false;
                        }).build();
                }).build();
            ui.text("settings.theme.hint")
                .position(contentX + inset, hintY).size(contentW - inset * 2.0f, 34.0f)
                .text(tr(UiText::ThemeHint)).fontSize(kFontLabel).color(theme.hintText).wrap(true).build();
        })
        .build();
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

    const float panelW = std::min(480.0f, w);
    const float panelX = x + std::max(0.0f, (w - panelW) * 0.5f);
    const float panelY = y + std::max(16.0f, (h - 230.0f) * 0.2f);
    ui.text("project.settings.title")
        .position(panelX, panelY).size(panelW, 26.0f).text("项目设置")
        .fontSize(kFontBody + 2.0f).lineHeight(26.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    ui.rect("project.settings.panel")
        .position(panelX, panelY + 36.0f).size(panelW, 168.0f)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.56f : 0.72f))
        .radius(kPanelRadius).build();
    ui.text("project.settings.org.label")
        .position(panelX + 16.0f, panelY + 52.0f).size(88.0f, 24.0f)
        .text("所属组织").fontSize(kFontLabel).lineHeight(24.0f).color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center).build();
    ui.text("project.settings.org.value")
        .position(panelX + 112.0f, panelY + 52.0f).size(panelW - 128.0f, 24.0f)
        .text(currentOrgName(project->orgId)).fontSize(kFontBody).lineHeight(24.0f)
        .color(theme.bodyText).verticalAlign(core::VerticalAlign::Center).build();
    ui.text("project.settings.name.label")
        .position(panelX + 16.0f, panelY + 84.0f).size(88.0f, kInputHeight)
        .text("项目名称").fontSize(kFontLabel).lineHeight(kInputHeight).color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center).build();
    components::input(ui, "project.settings.name")
        .position(panelX + 112.0f, panelY + 84.0f).size(panelW - 128.0f, kInputHeight)
        .value(g_projectNameDraft).theme(tokens)
        .onChange([](const std::string& value) { g_projectNameDraft = value; })
        .onEnter([id = project->id] {
            const std::string name = trim(g_projectNameDraft);
            if (name.empty()) { showStatus("项目名称不能为空"); return; }
            const std::string err = g_requests.renameProject(id, name);
            showStatus(err.empty() ? "项目设置已保存" : ("保存失败: " + err));
        })
        .build();
    components::button(ui, "project.settings.environments")
        .position(panelX + 16.0f, panelY + 126.0f).size(102.0f, 24.0f)
        .icon(0xF013).text("环境管理").fontSize(kFontLabel).iconSize(8.0f)
        .theme(tokens, false)
        .onClick([] { g_envManageOpen = true; })
        .build();
    components::button(ui, "project.settings.save")
        .position(panelX + panelW - 90.0f, panelY + 126.0f).size(74.0f, 24.0f)
        .text("保存").fontSize(kFontLabel).theme(tokens, true)
        .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
        .onClick([id = project->id] {
            const std::string name = trim(g_projectNameDraft);
            if (name.empty()) { showStatus("项目名称不能为空"); return; }
            const std::string err = g_requests.renameProject(id, name);
            showStatus(err.empty() ? "项目设置已保存" : ("保存失败: " + err));
        })
        .build();
}
