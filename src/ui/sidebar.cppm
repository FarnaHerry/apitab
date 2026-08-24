// ui/sidebar.cppm — 集合侧栏：当前项目的已保存请求，按分组层级展示。
// 点击请求 = 打开新标签页或切换到已有标签页；头部 + = 新建草稿标签页。
// 分组有两种模式：
//   仅名称 — 分组只作组织展示，不参与 URL；
//   路径   — 分组名是 API 路径前缀（如 "api/v1"），其下请求 URL 自动拼前缀。
module;

#include "eui_ui.h"

export module apitab.ui.sidebar;

import std;
import apitab.db;
import apitab.store.requests;  // g_requests
import apitab.store.ui;        // openTab / newDraftTab / closeTab / showStatus / askConfirm
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

// 侧栏列表滚动位置（视图状态，模块私有 —— 无人跨模块用）。
float g_sidebarScroll = 0.0f;

namespace {

// 分组段（section header）高度。
constexpr float kGroupRowH = 22.0f;
// 请求行高。
constexpr float kReqRowH = 26.0f;

// 分组切换弹窗里的模式选项索引（与 db::GroupMode 对齐）。
constexpr int kModeName = 0;
constexpr int kModePath = 1;

void deleteRequest(std::int64_t requestId) {
    std::vector<std::int64_t> uids;
    for (const auto& tab : g_tabs) {
        if (tab.requestId == requestId) uids.push_back(tab.uid);
    }
    for (const std::int64_t uid : uids) closeTab(uid);
    const std::string err = g_requests.remove(requestId);
    showStatus(err.empty() ? "已删除" : ("删除失败: " + err));
}

void drawRenameRequestDialog(eui::Ui& ui, const eui::Screen& screen,
                             const AppTheme& theme) {
    if (!g_renameRequestOpen) return;
    components::dialog(ui, "sidebar.request.rename")
        .open(true)
        .screen(screen.width, screen.height)
        .size(360.0f, 154.0f)
        .title("重命名请求")
        .theme(theme.components)
        .content([&] {
            components::input(ui, "sidebar.request.rename.input")
                .position(20.0f, 56.0f)
                .size(320.0f, kInputHeight)
                .value(g_requestRenameText)
                .placeholder("请求名称")
                .theme(theme.components)
                .onChange([](const std::string& v) { g_requestRenameText = v; })
                .build();
            components::button(ui, "sidebar.request.rename.cancel")
                .position(184.0f, 104.0f)
                .size(74.0f, 24.0f)
                .text("取消")
                .fontSize(kFontLabel)
                .theme(theme.components, false)
                .onClick([] {
                    g_renameRequestOpen = false;
                    g_renameRequestId = 0;
                    g_requestRenameText.clear();
                })
                .build();
            components::button(ui, "sidebar.request.rename.confirm")
                .position(266.0f, 104.0f)
                .size(74.0f, 24.0f)
                .text("确定")
                .fontSize(kFontLabel)
                .theme(theme.components, true)
                .textColor(onPrimaryColor(theme))
                .iconColor(onPrimaryColor(theme))
                .onClick([] {
                    const std::string name = trim(g_requestRenameText);
                    if (name.empty()) {
                        showStatus("请求名称不能为空");
                        return;
                    }
                    const db::SavedRequest* saved = g_requests.find(g_renameRequestId);
                    if (!saved) {
                        showStatus("请求不存在或已删除");
                        g_renameRequestOpen = false;
                        g_renameRequestId = 0;
                        return;
                    }
                    db::SavedRequest renamed = *saved;
                    renamed.name = name;
                    const std::string err = g_requests.save(renamed);
                    if (!err.empty()) {
                        showStatus("重命名失败: " + err);
                        return;
                    }
                    for (auto& tab : g_tabs) {
                        if (tab.requestId == renamed.id) tab.draft.name = name;
                    }
                    g_renameRequestOpen = false;
                    g_renameRequestId = 0;
                    g_requestRenameText.clear();
                    showStatus("已重命名: " + name);
                })
                .build();
        })
        .build();
}

void drawNewGroupDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    if (!g_newGroupOpen) return;
    components::dialog(ui, "sidebar.group.new")
        .open(true)
        .screen(screen.width, screen.height)
        .size(360.0f, 200.0f)
        .title("新建分组")
        .theme(theme.components)
        .content([&] {
            const float cx = 20.0f;
            const float cy = 56.0f;
            const float cw = 320.0f;
            components::input(ui, "sidebar.group.new.name")
                .position(cx, cy)
                .size(cw, kInputHeight)
                .value(g_newGroupText)
                .placeholder("分组名（路径模式可用 / 分隔，如 api/v1）")
                .theme(theme.components)
                .onChange([](const std::string& v) { g_newGroupText = v; })
                .build();
            ui.stack("sidebar.group.new.mode.wrap")
                .position(cx, cy + kInputHeight + 10.0f)
                .size(cw, 24.0f)
                .content([&] {
                    components::segmented(ui, "sidebar.group.new.mode")
                        .size(cw, 24.0f)
                        .items({"仅名称", "路径"})
                        .selected(g_newGroupMode)
                        .theme(theme.components)
                        .style(segmentedStyle(theme))
                        .onChange([](int i) { g_newGroupMode = i; })
                        .build();
                })
                .build();
            ui.text("sidebar.group.new.hint")
                .position(cx, cy + kInputHeight + 40.0f)
                .size(cw, 28.0f)
                .text("路径模式：分组名作为 URL 前缀，请求只填相对路径")
                .fontSize(kFontLabel)
                .color(theme.hintText)
                .wrap(true)
                .build();
            // 自定义 content 会替换 Dialog 默认操作区，因此按钮必须显式绘制。
            components::button(ui, "sidebar.group.new.cancel")
                .position(cx + 164.0f, cy + kInputHeight + 70.0f)
                .size(74.0f, 24.0f)
                .text("取消")
                .fontSize(kFontLabel)
                .theme(theme.components, false)
                .onClick([] {
                    g_newGroupOpen = false;
                    g_newGroupText.clear();
                    g_newGroupMode = kModeName;
                })
                .build();
            components::button(ui, "sidebar.group.new.create")
                .position(cx + 246.0f, cy + kInputHeight + 70.0f)
                .size(74.0f, 24.0f)
                .text("创建")
                .fontSize(kFontLabel)
                .theme(theme.components, true)
                .textColor(onPrimaryColor(theme))
                .iconColor(onPrimaryColor(theme))
                .onClick([] {
                    const std::string name = trim(g_newGroupText);
                    if (name.empty()) {
                        showStatus("分组名不能为空");
                        return;
                    }
                    const auto mode = g_newGroupMode == kModePath ? db::GroupMode::Path
                                                                  : db::GroupMode::Name;
                    const std::string err = g_requests.createGroup(name, mode);
                    if (!err.empty()) {
                        showStatus("创建分组失败: " + err);
                        return;
                    }
                    g_newGroupOpen = false;
                    g_newGroupText.clear();
                    g_newGroupMode = kModeName;
                })
                .build();
        })
        .build();
}

void drawGroupRow(eui::Ui& cu, const std::string& rowId, float width,
                  const db::Group& group, const AppTheme& theme) {
    const bool collapsed = g_groupCollapsed.contains(group.id) && g_groupCollapsed.at(group.id);
    const bool renaming = (g_renamingGroupId == group.id);

    cu.stack(rowId)
        .size(width, kGroupRowH)
        .content([&] {
            // 命中区（整行可点，切换展开/收起）
            cu.rect(rowId + ".hit")
                .position(0, 0)
                .size(width, kGroupRowH)
                .states(core::Color{0, 0, 0, 0}, theme.components.surfaceHover,
                        theme.components.surfaceActive)
                .radius(5.0f)
                .onClick([&] {
                    g_groupCollapsed[group.id] = !collapsed;
                })
                .build();
            // 展开/收起箭头
            cu.text(rowId + ".arrow")
                .position(4.0f, 0)
                .size(14.0f, kGroupRowH)
                .text(collapsed ? "\xEF\x83\x9A" : "\xEF\x83\x97")  // ▸ / ▾
                .fontSize(9.0f)
                .lineHeight(kGroupRowH)
                .color(theme.metaText)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            // 文件夹图标 + 模式角标
            cu.text(rowId + ".icon")
                .position(18.0f, 0)
                .size(14.0f, kGroupRowH)
                .text(group.mode == db::GroupMode::Path ? "\xEF\x84\xA1" : "\xEF\x81\xBB")  //  / 📁
                .fontSize(9.0f)
                .lineHeight(kGroupRowH)
                .color(theme.metaText)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            // 分组名（重命名时变输入框）
            if (renaming) {
                components::input(cu, rowId + ".rename")
                    .position(34.0f, 1.0f)
                    .size(width - 34.0f - 106.0f, kGroupRowH - 2.0f)
                    .value(g_groupRenameText)
                    .fontSize(kFontLabel)
                    .theme(theme.components)
                    .onChange([](const std::string& v) { g_groupRenameText = v; })
                    .onEnter([&] {
                        const std::string name = trim(g_groupRenameText);
                        if (!name.empty()) {
                            if (const std::string err = g_requests.renameGroup(group.id, name);
                                !err.empty()) {
                                showStatus("重命名失败: " + err);
                            }
                        }
                        g_renamingGroupId = 0;
                    })
                    .onFocus([&](bool focused) {
                        if (!focused) g_renamingGroupId = 0;
                    })
                    .build();
            } else {
                cu.text(rowId + ".name")
                    .position(34.0f, 0)
                    .size(width - 34.0f - 106.0f, kGroupRowH)
                    .text(group.name)
                    .fontSize(kFontBody)
                    .lineHeight(kGroupRowH)
                    .color(theme.titleText)
                    .verticalAlign(core::VerticalAlign::Center)
                    .build();
            }
            // 模式标签
            cu.text(rowId + ".mode")
                .position(width - 100.0f, 0)
                .size(28.0f, kGroupRowH)
                .text(db::groupModeName(group.mode))
                .fontSize(8.0f)
                .lineHeight(kGroupRowH)
                .color(theme.hintText)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            // 在此分组新建请求
            components::button(cu, rowId + ".add")
                .position(width - 72.0f, 2.0f)
                .size(16.0f, 18.0f)
                .icon(0xF067)
                .text("")
                .iconSize(7.0f)
                .theme(theme.components, false)
                .onClick([groupId = group.id] {
                    RequestTab& tab = newDraftTab();
                    tab.draft.groupId = groupId;
                    g_page = Page::Request;
                })
                .build();
            // 当前请求移入此分组
            components::button(cu, rowId + ".move")
                .position(width - 54.0f, 2.0f)
                .size(16.0f, 18.0f)
                .icon(0xF061)
                .text("")
                .iconSize(7.0f)
                .theme(theme.components, false)
                .onClick([groupId = group.id, groupName = group.name] {
                    RequestTab& tab = activeTab();
                    tab.draft.groupId = groupId;
                    if (tab.requestId == 0) {
                        showStatus("新请求将保存到: " + groupName);
                        return;
                    }
                    const std::string err = g_requests.moveToGroup(tab.requestId, groupId);
                    showStatus(err.empty() ? "已移动到: " + groupName
                                           : "移动失败: " + err);
                })
                .build();
            // 重命名按钮
            components::button(cu, rowId + ".edit")
                .position(width - 36.0f, 2.0f)
                .size(16.0f, 18.0f)
                .icon(0xF303)  // fa-pen
                .text("")
                .iconSize(7.0f)
                .theme(theme.components, false)
                .onClick([&] {
                    g_groupRenameText = group.name;
                    g_renamingGroupId = group.id;
                })
                .build();
            // 删除按钮（弹确认）
            components::button(cu, rowId + ".del")
                .position(width - 18.0f, 2.0f)
                .size(16.0f, 18.0f)
                .icon(0xF1F8)  // fa-trash
                .text("")
                .iconSize(7.0f)
                .theme(theme.components, false)
                .onClick([&] {
                    askConfirm("删除分组",
                               std::format("将删除分组「{}」，其下请求移为未分组。", group.name),
                               [&] {
                                   const std::string err = g_requests.deleteGroup(group.id);
                                   showStatus(err.empty() ? "分组已删除" : ("删除失败: " + err));
                               });
                })
                .build();
        })
        .build();
}

void drawRequestRow(eui::Ui& cu, const std::string& rowId, float width,
                    const db::SavedRequest& r, const AppTheme& theme) {
    const bool selected = (activeTab().requestId == r.id);
    const std::int64_t rid = r.id;

    cu.stack(rowId)
        .size(width, kReqRowH)
        .content([&] {
            cu.rect(rowId + ".bg")
                .position(0, 0)
                .size(width, kReqRowH)
                .color(selected ? components::theme::withAlpha(theme.components.primary, 0.18f)
                                : core::Color{0, 0, 0, 0})
                .radius(6.0f)
                .build();
            cu.rect(rowId + ".hit")
                .position(0, 0)
                .size(width, kReqRowH)
                .states(core::Color{0, 0, 0, 0}, theme.components.surfaceHover,
                        theme.components.surfaceActive)
                .radius(6.0f)
                .onClick([&r] {
                    (void)openTab(r);
                    g_page = Page::Request;
                })
                .onContextMenu([rid](const eui::PointerEvent& event, const eui::Rect&) {
                    g_collectionMenuOpen = false;
                    g_requestMenuTargetId = rid;
                    g_requestMenuX = static_cast<float>(event.x);
                    g_requestMenuY = static_cast<float>(event.y);
                    g_requestMenuOpen = true;
                })
                .build();
            // 方法徽章（固定宽，着色）
            cu.text(rowId + ".method")
                .position(6.0f, 0)
                .size(38.0f, kReqRowH)
                .text(r.method)
                .fontSize(9.0f)
                .lineHeight(kReqRowH)
                .color(methodColor(r.method, theme))
                .verticalAlign(core::VerticalAlign::Center)
                .build();
            cu.text(rowId + ".name")
                .position(46.0f, 0)
                .size(width - 52.0f, kReqRowH)
                .text(r.name)
                .fontSize(kFontBody)
                .lineHeight(kReqRowH)
                .color(theme.bodyText)
                .verticalAlign(core::VerticalAlign::Center)
                .build();
        })
        .build();
}

} // namespace

// 侧栏 + 新建分组弹窗（弹窗需要全屏坐标，由 app.cpp 传入 screen）。
export void drawSidebar(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    const auto& tokens = theme.components;
    const float height = screen.height;
    const float x0 = kRailWidth;
    const float w = kSidebarWidth;

    // 底面板
    ui.rect("sidebar.bg")
        .position(x0, 0)
        .size(w, height - 20.0f)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.35f : 0.6f))
        .build();

    // 标题行 + 新建请求。标题区域右键打开集合菜单。
    ui.rect("sidebar.title.context")
        .position(x0, 0)
        .size(w, 36.0f)
        .color(core::Color{0, 0, 0, 0})
        .onContextMenu([](const eui::PointerEvent& event, const eui::Rect&) {
            g_collectionMenuOpen = true;
            g_collectionMenuX = static_cast<float>(event.x);
            g_collectionMenuY = static_cast<float>(event.y);
        })
        .build();
    ui.text("sidebar.title")
        .position(x0 + kMargin, 8.0f)
        .size(w - kMargin * 2.0f - 30.0f, 22.0f)
        .text("请求集合")
        .fontSize(kFontBody + 1.0f)
        .lineHeight(22.0f)
        .color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center)
        .build();
    components::button(ui, "sidebar.new")
        .position(x0 + w - kMargin - 24.0f, 8.0f)
        .size(24.0f, 22.0f)
        .icon(0xF067)  // fa-plus
        .text("")
        .iconSize(10.0f)
        .theme(tokens, false)
        .onClick([] {
            (void)newDraftTab();
            g_page = Page::Request;
        })
        .build();

    // 请求列表（scrollView；按分组分段，分组内请求缩进）
    const auto& items = g_requests.list();
    const auto& groups = g_requests.groups();
    const float listY = 36.0f;
    const float listH = height - 20.0f - listY - 4.0f;

    components::scrollView(ui, "sidebar.list")
        .position(x0, listY)
        .size(w, listH)
        .offset(g_sidebarScroll)
        .step(40.0f)
        .theme(tokens)
        .onChange([](float v) { g_sidebarScroll = v; })
        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
            if (items.empty() && groups.empty()) {
                cu.stack("sidebar.empty.row")
                    .size(contentWidth - 8.0f, 56.0f)
                    .content([&] {
                        cu.text("sidebar.empty")
                            .position(kMargin, 8.0f)
                            .size(contentWidth - kMargin * 2.0f, 40.0f)
                            .text("还没有保存的请求\n点右上角 + 新建")
                            .fontSize(kFontLabel)
                            .color(theme.hintText)
                            .build();
                    })
                    .build();
                cu.stack("sidebar.blank.context")
                    .size(contentWidth - 8.0f, std::max(kReqRowH, viewportH - 56.0f))
                    .content([&] {
                        cu.rect("sidebar.blank.context.hit")
                            .size(contentWidth - 8.0f, std::max(kReqRowH, viewportH - 56.0f))
                            .color(core::Color{0, 0, 0, 0})
                            .onContextMenu([](const eui::PointerEvent& event, const eui::Rect&) {
                                g_requestMenuOpen = false;
                                g_collectionMenuOpen = true;
                                g_collectionMenuX = static_cast<float>(event.x);
                                g_collectionMenuY = static_cast<float>(event.y);
                            })
                            .build();
                    })
                    .build();
                return;
            }

            // 1) 有分组的请求，按分组分段展示
            for (const auto& group : groups) {
                const std::string groupId = "sidebar.grp." + std::to_string(group.id);
                const bool collapsed =
                    g_groupCollapsed.contains(group.id) && g_groupCollapsed.at(group.id);

                drawGroupRow(cu, groupId, contentWidth - 8.0f, group, theme);

                if (!collapsed) {
                    for (const auto& r : items) {
                        if (r.groupId != group.id) continue;
                        // 分组内请求缩进 14px（层级感）
                        cu.stack(groupId + ".indent." + std::to_string(r.id))
                            .size(contentWidth - 8.0f, kReqRowH)
                            .content([&] {
                                // 缩进占位（左空 14px），实际行宽减缩进
                                drawRequestRow(cu, groupId + ".req." + std::to_string(r.id),
                                               contentWidth - 8.0f - 14.0f, r, theme);
                            })
                            .build();
                    }
                }
            }

            // 2) 未分组请求（group_id == 0）
            bool hasUngrouped = false;
            for (const auto& r : items) {
                if (r.groupId == 0) hasUngrouped = true;
            }
            if (hasUngrouped && !groups.empty()) {
                const float ungroupedW = contentWidth - 8.0f;
                cu.stack("sidebar.ungrouped.row")
                    .size(ungroupedW, kGroupRowH)
                    .content([&] {
                        cu.text("sidebar.ungrouped.label")
                            .position(4.0f, 0)
                            .size(ungroupedW - 26.0f, kGroupRowH)
                            .text("未分组")
                            .fontSize(kFontLabel)
                            .lineHeight(kGroupRowH)
                            .color(theme.hintText)
                            .verticalAlign(core::VerticalAlign::Center)
                            .build();
                        components::button(cu, "sidebar.ungrouped.move")
                            .position(ungroupedW - 18.0f, 2.0f)
                            .size(16.0f, 18.0f)
                            .icon(0xF060)  // fa-arrow-left
                            .text("")
                            .iconSize(7.0f)
                            .theme(theme.components, false)
                            .onClick([] {
                                RequestTab& tab = activeTab();
                                tab.draft.groupId = 0;
                                if (tab.requestId == 0) {
                                    showStatus("新请求将保存为未分组");
                                    return;
                                }
                                const std::string err = g_requests.moveToGroup(tab.requestId, 0);
                                showStatus(err.empty() ? "已移出分组" : "移动失败: " + err);
                            })
                            .build();
                    })
                    .build();
            }
            for (const auto& r : items) {
                if (r.groupId != 0) continue;
                drawRequestRow(cu, "sidebar.req." + std::to_string(r.id),
                               contentWidth - 8.0f, r, theme);
            }
            cu.stack("sidebar.blank.context")
                .size(contentWidth - 8.0f, std::max(kReqRowH, viewportH))
                .content([&] {
                    cu.rect("sidebar.blank.context.hit")
                        .size(contentWidth - 8.0f, std::max(kReqRowH, viewportH))
                        .color(core::Color{0, 0, 0, 0})
                        .onContextMenu([](const eui::PointerEvent& event, const eui::Rect&) {
                            g_requestMenuOpen = false;
                            g_collectionMenuOpen = true;
                            g_collectionMenuX = static_cast<float>(event.x);
                            g_collectionMenuY = static_cast<float>(event.y);
                        })
                        .build();
                })
                .build();
        })
        .build();

    components::contextMenu(ui, "sidebar.collection.menu")
        .open(g_collectionMenuOpen)
        .screen(screen.width, screen.height)
        .position(g_collectionMenuX, g_collectionMenuY)
        .size(150.0f, 26.0f)
        .items({"新建请求", "新建分组"})
        .theme(tokens)
        .zIndex(100)
        .onSelect([](int index) {
            g_collectionMenuOpen = false;
            if (index == 0) {
                (void)newDraftTab();
                g_page = Page::Request;
            } else if (index == 1) {
                g_newGroupOpen = true;
            }
        })
        .onOpenChange([](bool open) { g_collectionMenuOpen = open; })
        .build();

    components::contextMenu(ui, "sidebar.request.menu")
        .open(g_requestMenuOpen)
        .screen(screen.width, screen.height)
        .position(g_requestMenuX, g_requestMenuY)
        .size(150.0f, 26.0f)
        .items({"重命名", "删除"})
        .theme(tokens)
        .zIndex(101)
        .onSelect([](int index) {
            const std::int64_t requestId = g_requestMenuTargetId;
            g_requestMenuOpen = false;
            g_requestMenuTargetId = 0;
            const db::SavedRequest* request = g_requests.find(requestId);
            if (!request) {
                showStatus("请求不存在或已删除");
                return;
            }
            if (index == 0) {
                g_renameRequestId = requestId;
                g_requestRenameText = request->name;
                g_renameRequestOpen = true;
            } else if (index == 1) {
                const std::string name = request->name;
                askConfirm("删除请求", std::format("将删除请求「{}」。", name),
                           [requestId] { deleteRequest(requestId); });
            }
        })
        .onOpenChange([](bool open) {
            g_requestMenuOpen = open;
            if (!open) g_requestMenuTargetId = 0;
        })
        .build();
}
// 新建分组和请求重命名弹窗（需要全屏坐标系，在 app.cpp 里独立于侧栏绘制）。
export void drawSidebarDialogs(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    drawNewGroupDialog(ui, screen, theme);
    drawRenameRequestDialog(ui, screen, theme);
}
