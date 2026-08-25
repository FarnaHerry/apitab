// ui/sidebar.cppm — 当前项目请求集合与目录树。
module;

#include "eui_ui.h"
#include "context_scroll_view.h"

export module apitab.ui.sidebar;

import std;
import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.store.tcp;
import apitab.store.websocket;
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

float g_sidebarScroll = 0.0f;

namespace {

constexpr float kGroupRowH = 22.0f;
constexpr float kReqRowH = 26.0f;
constexpr int kModeName = 0;
constexpr int kModePath = 1;

void openRequestTypeMenu(float x, float y, std::int64_t groupId = 0) {
    g_collectionMenuOpen = false;
    g_requestMenuOpen = false;
    g_groupMenuOpen = false;
    g_requestTypeMenuX = x;
    g_requestTypeMenuY = y;
    g_requestTypeGroupId = groupId;
    g_requestTypeMenuOpen = true;
}

void createHttpDraft(std::int64_t groupId) {
    (void)newDraftTab(api::RequestKind::Http, groupId);
    g_page = Page::Request;
}

void createWebSocketDraft(std::int64_t groupId) {
    (void)newDraftTab(api::RequestKind::WebSocket, groupId);
    g_page = Page::Request;
}

void createTcpDraft(api::RequestKind kind, std::int64_t groupId) {
    (void)newDraftTab(kind, groupId);
    g_page = Page::Request;
}

std::string savedBadge(const db::SavedRequest& request) {
    switch (request.kind) {
        case api::RequestKind::Http: return request.method;
        case api::RequestKind::WebSocket: return "WS";
        case api::RequestKind::Tcp: return "TCP";
    }
    return "?";
}

std::string truncateLabel(const std::string& value, float maxWidth, float fontSize) {
    if (maxWidth <= 0.0f) return {};
    if (core::TextPrimitive::measureTextWidth(value, {}, fontSize, 400) <= maxWidth) return value;
    constexpr std::string_view ellipsis = "…";
    const float ellipsisWidth = core::TextPrimitive::measureTextWidth(
        std::string(ellipsis), {}, fontSize, 400);
    if (ellipsisWidth > maxWidth) return {};
    std::size_t end = 0;
    while (end < value.size()) {
        ++end;
        while (end < value.size() && (static_cast<unsigned char>(value[end]) & 0xC0u) == 0x80u) ++end;
        const std::string candidate = value.substr(0, end) + std::string(ellipsis);
        if (core::TextPrimitive::measureTextWidth(candidate, {}, fontSize, 400) > maxWidth) break;
    }
    if (end == 0) return std::string(ellipsis);
    while (end > 0) {
        const std::string candidate = value.substr(0, end) + std::string(ellipsis);
        if (core::TextPrimitive::measureTextWidth(candidate, {}, fontSize, 400) <= maxWidth) {
            return candidate;
        }
        --end;
        while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0u) == 0x80u) --end;
    }
    return {};
}

void deleteRequest(std::int64_t requestId) {
    std::vector<std::int64_t> uids;
    for (const auto& tab : g_tabs) {
        if (tab.requestId == requestId) uids.push_back(tab.uid);
    }
    for (const std::int64_t uid : uids) {
        g_websocket.release(uid);
        g_tcp.release(uid);
        closeTab(uid);
    }
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
                .position(20.0f, 56.0f).size(320.0f, kInputHeight)
                .value(g_requestRenameText).placeholder("请求名称").theme(theme.components)
                .onChange([](const std::string& value) { g_requestRenameText = value; }).build();
            components::button(ui, "sidebar.request.rename.cancel")
                .position(184.0f, 104.0f).size(74.0f, 24.0f)
                .text("取消").fontSize(kFontLabel).theme(theme.components, false)
                .onClick([] {
                    g_renameRequestOpen = false;
                    g_renameRequestId = 0;
                    g_requestRenameText.clear();
                }).build();
            components::button(ui, "sidebar.request.rename.confirm")
                .position(266.0f, 104.0f).size(74.0f, 24.0f)
                .text("确定").fontSize(kFontLabel).theme(theme.components, true)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                .onClick([] {
                    const std::string name = trim(g_requestRenameText);
                    if (name.empty()) { showStatus("请求名称不能为空"); return; }
                    const db::SavedRequest* saved = g_requests.find(g_renameRequestId);
                    if (!saved) { showStatus("请求不存在或已删除"); return; }
                    db::SavedRequest renamed = *saved;
                    renamed.name = name;
                    const std::string err = g_requests.save(renamed);
                    if (!err.empty()) { showStatus("重命名失败: " + err); return; }
                    for (auto& tab : g_tabs) {
                        if (tab.requestId == renamed.id) tab.draft.name = name;
                    }
                    g_renameRequestOpen = false;
                    g_renameRequestId = 0;
                    g_requestRenameText.clear();
                    showStatus("已重命名: " + name);
                }).build();
        }).build();
}

void drawNewGroupDialog(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    if (!g_newGroupOpen) return;
    const bool child = g_newGroupParentId != 0;
    components::dialog(ui, "sidebar.group.new")
        .open(true).screen(screen.width, screen.height).size(360.0f, 200.0f)
        .title(child ? "新建子目录" : "新建分组").theme(theme.components)
        .content([&] {
            constexpr float cx = 20.0f;
            constexpr float cy = 56.0f;
            constexpr float cw = 320.0f;
            components::input(ui, "sidebar.group.new.name")
                .position(cx, cy).size(cw, kInputHeight).value(g_newGroupText)
                .placeholder("目录名称").theme(theme.components)
                .onChange([](const std::string& value) { g_newGroupText = value; }).build();
            ui.stack("sidebar.group.new.mode.wrap")
                .position(cx, cy + kInputHeight + 10.0f).size(cw, 24.0f)
                .content([&] {
                    components::segmented(ui, "sidebar.group.new.mode")
                        .size(cw, 24.0f).items({"仅名称", "路径"}).selected(g_newGroupMode)
                        .theme(theme.components).style(segmentedStyle(theme))
                        .onChange([](int index) { g_newGroupMode = index; }).build();
                }).build();
            ui.text("sidebar.group.new.hint")
                .position(cx, cy + kInputHeight + 40.0f).size(cw, 28.0f)
                .text("路径模式：此目录名作为 URL 前缀")
                .fontSize(kFontLabel).color(theme.hintText).wrap(true).build();
            components::button(ui, "sidebar.group.new.cancel")
                .position(cx + 164.0f, cy + kInputHeight + 70.0f).size(74.0f, 24.0f)
                .text("取消").fontSize(kFontLabel).theme(theme.components, false)
                .onClick([] {
                    g_newGroupOpen = false;
                    g_newGroupText.clear();
                    g_newGroupMode = kModeName;
                    g_newGroupParentId = 0;
                }).build();
            components::button(ui, "sidebar.group.new.create")
                .position(cx + 246.0f, cy + kInputHeight + 70.0f).size(74.0f, 24.0f)
                .text("创建").fontSize(kFontLabel).theme(theme.components, true)
                .textColor(onPrimaryColor(theme)).iconColor(onPrimaryColor(theme))
                .onClick([] {
                    const std::string name = trim(g_newGroupText);
                    if (name.empty()) { showStatus("目录名称不能为空"); return; }
                    const auto mode = g_newGroupMode == kModePath ? db::GroupMode::Path
                                                                  : db::GroupMode::Name;
                    const std::string err = g_requests.createGroup(name, mode, g_newGroupParentId);
                    if (!err.empty()) { showStatus("创建目录失败: " + err); return; }
                    g_newGroupOpen = false;
                    g_newGroupText.clear();
                    g_newGroupMode = kModeName;
                    g_newGroupParentId = 0;
                }).build();
        }).build();
}

void drawGroupRow(eui::Ui& ui, const std::string& rowId, float width, float indent,
                  const db::Group& group, const AppTheme& theme) {
    const std::int64_t groupId = group.id;
    const bool collapsed = g_groupCollapsed.contains(groupId) && g_groupCollapsed.at(groupId);
    const bool renaming = g_renamingGroupId == groupId;
    const float rowW = std::max(0.0f, width);
    const float menuX = std::max(0.0f, rowW - 22.0f);
    const float textX = indent + 34.0f;
    const float textW = std::max(0.0f, rowW - textX - 54.0f);

    ui.stack(rowId).size(rowW, kGroupRowH).content([&] {
        ui.rect(rowId + ".hit")
            .size(rowW, kGroupRowH)
            .states(core::Color{0, 0, 0, 0}, theme.components.surfaceHover,
                    theme.components.surfaceActive)
            .radius(5.0f)
            .onClick([groupId] {
                g_groupCollapsed[groupId] = !g_groupCollapsed[groupId];
            }).build();
        ui.text(rowId + ".arrow")
            .position(indent + 4.0f, 0).size(14.0f, kGroupRowH)
            .icon(collapsed ? 0xF054 : 0xF078).fontSize(8.0f).lineHeight(kGroupRowH)
            .color(theme.metaText).verticalAlign(core::VerticalAlign::Center).build();
        ui.text(rowId + ".icon")
            .position(indent + 18.0f, 0).size(14.0f, kGroupRowH)
            .icon(group.mode == db::GroupMode::Path ? 0xF07C : 0xF07B)
            .fontSize(9.0f).lineHeight(kGroupRowH).color(theme.metaText)
            .verticalAlign(core::VerticalAlign::Center).build();
        if (renaming) {
            components::input(ui, rowId + ".rename")
                .position(textX, 1.0f).size(textW, kGroupRowH - 2.0f)
                .value(g_groupRenameText).fontSize(kFontLabel).theme(theme.components)
                .onChange([](const std::string& value) { g_groupRenameText = value; })
                .onEnter([groupId] {
                    const std::string name = trim(g_groupRenameText);
                    if (!name.empty()) {
                        const std::string err = g_requests.renameGroup(groupId, name);
                        if (!err.empty()) showStatus("重命名失败: " + err);
                    }
                    g_renamingGroupId = 0;
                })
                .onFocus([](bool focused) { if (!focused) g_renamingGroupId = 0; })
                .build();
        } else {
            ui.text(rowId + ".name")
                .position(textX, 0).size(textW, kGroupRowH).text(truncateLabel(group.name, textW, kFontBody))
                .fontSize(kFontBody).lineHeight(kGroupRowH).color(theme.titleText)
                .verticalAlign(core::VerticalAlign::Center).build();
        }
        ui.text(rowId + ".mode")
            .position(width - 52.0f, 0).size(26.0f, kGroupRowH)
            .text(db::groupModeName(group.mode)).fontSize(8.0f).lineHeight(kGroupRowH)
            .color(theme.hintText).verticalAlign(core::VerticalAlign::Center).build();
        components::button(ui, rowId + ".menu")
            .position(menuX, 2.0f).size(18.0f, 18.0f).icon(0xF141).text("")
            .iconSize(8.0f).theme(theme.components, false)
            .build();
        components::mouseArea(ui, rowId + ".menu.hit")
            .position(menuX, 2.0f).size(18.0f, 18.0f).zIndex(1)
            .onTap([groupId](const components::MouseEvent& event) {
                g_collectionMenuOpen = false;
                g_requestMenuOpen = false;
                g_requestTypeMenuOpen = false;
                g_groupMenuTargetId = groupId;
                g_groupMenuX = event.bounds.x;
                g_groupMenuY = event.bounds.y + event.bounds.height;
                g_groupMenuOpen = true;
            }).build();
    }).build();
}

void drawRequestRow(eui::Ui& ui, const std::string& rowId, float width, float indent,
                    const db::SavedRequest& request, const AppTheme& theme) {
    const std::int64_t requestId = request.id;
    const bool selected = activeTab().requestId == requestId;
    const float rowW = std::max(0.0f, width);
    const float nameX = indent + 46.0f;
    const float nameW = std::max(0.0f, rowW - nameX - 6.0f);
    ui.stack(rowId).size(rowW, kReqRowH).content([&] {
        ui.rect(rowId + ".bg").size(rowW, kReqRowH)
            .color(selected ? components::theme::withAlpha(theme.components.primary, 0.18f)
                            : core::Color{0, 0, 0, 0})
            .radius(6.0f).build();
        ui.rect(rowId + ".hit").size(rowW, kReqRowH)
            .states(core::Color{0, 0, 0, 0}, theme.components.surfaceHover,
                    theme.components.surfaceActive)
            .radius(6.0f)
            .onClick([requestId] {
                if (const db::SavedRequest* saved = g_requests.find(requestId)) (void)openTab(*saved);
                g_page = Page::Request;
            })
            .onContextMenu([requestId](const eui::PointerEvent& event, const eui::Rect&) {
                g_collectionMenuOpen = false;
                g_groupMenuOpen = false;
                g_requestMenuTargetId = requestId;
                g_requestMenuX = static_cast<float>(event.x);
                g_requestMenuY = static_cast<float>(event.y);
                g_requestMenuOpen = true;
            }).build();
        ui.text(rowId + ".method")
            .text(savedBadge(request))
            .fontSize(9.0f).lineHeight(kReqRowH)
            .color(request.kind == api::RequestKind::Http ? methodColor(request.method, theme)
                   : request.kind == api::RequestKind::WebSocket ? theme.redirect : theme.clientErr)
            .verticalAlign(core::VerticalAlign::Center).build();
        ui.text(rowId + ".name")
            .position(nameX, 0).size(nameW, kReqRowH)
            .text(truncateLabel(request.name, nameW, kFontBody)).fontSize(kFontBody).lineHeight(kReqRowH).color(theme.bodyText)
            .verticalAlign(core::VerticalAlign::Center).build();
    }).build();
}

} // namespace

export void drawSidebar(eui::Ui& ui, const eui::Screen& screen, float top,
                        const AppTheme& theme) {
    const auto& tokens = theme.components;
    const float height = screen.height;
    const float x0 = kRailWidth;
    const float w = kSidebarWidth;

    ui.rect("sidebar.bg").position(x0, top).size(w, height - 20.0f - top)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.35f : 0.6f)).build();

    auto openCollectionMenu = [](const eui::PointerEvent& event, const eui::Rect&) {
        g_requestMenuOpen = false;
        g_groupMenuOpen = false;
        g_collectionMenuOpen = true;
        g_collectionMenuX = static_cast<float>(event.x);
        g_collectionMenuY = static_cast<float>(event.y);
    };

    ui.rect("sidebar.title.context").position(x0, top).size(w, 36.0f)
        .color(core::Color{0, 0, 0, 0}).onContextMenu(openCollectionMenu).build();
    ui.text("sidebar.title").position(x0 + kMargin, top + 8.0f)
        .size(w - kMargin * 2.0f - 30.0f, 22.0f).text("请求集合")
        .fontSize(kFontBody + 1.0f).lineHeight(22.0f).color(theme.titleText)
        .verticalAlign(core::VerticalAlign::Center).build();
    const float newX = x0 + w - kMargin - 24.0f;
    components::button(ui, "sidebar.new")
        .position(newX, top + 8.0f).size(24.0f, 22.0f)
        .icon(0xF067).text("").iconSize(10.0f).theme(tokens, false)
        .build();
    components::mouseArea(ui, "sidebar.new.hit")
        .position(newX, top + 8.0f).size(24.0f, 22.0f).zIndex(1)
        .onTap([](const components::MouseEvent& event) {
            openRequestTypeMenu(event.bounds.x, event.bounds.y + event.bounds.height);
        }).build();

    const auto& items = g_requests.list();
    const auto& groups = g_requests.groups();
    const float listY = top + 36.0f;
    const float listH = height - 20.0f - listY - 4.0f;
    apitab_components::contextScrollView(ui, "sidebar.list")
        .position(x0, listY).size(std::max(0.0f, w), std::max(0.0f, listH)).offset(g_sidebarScroll).step(40.0f).theme(tokens)
        .onChange([](float value) { g_sidebarScroll = value; })
        .onContextMenu(openCollectionMenu)
        .content([&](eui::Ui& content, float contentWidth, float) {
            if (items.empty() && groups.empty()) {
                content.stack("sidebar.empty.row").size(contentWidth - 8.0f, 56.0f).content([&] {
                    content.text("sidebar.empty").position(kMargin, 8.0f)
                        .size(contentWidth - kMargin * 2.0f, 40.0f)
                        .text("还没有保存的请求\n点右上角 + 新建").fontSize(kFontLabel)
                        .color(theme.hintText).build();
                }).build();
                return;
            }
            std::function<void(std::int64_t, float)> drawTree;
            drawTree = [&](std::int64_t parentId, float indent) {
                for (const auto& group : groups) {
                    if (group.parentId != parentId) continue;
                    const std::string groupRow = "sidebar.grp." + std::to_string(group.id);
                    drawGroupRow(content, groupRow, contentWidth - 8.0f, indent, group, theme);
                    const bool collapsed = g_groupCollapsed.contains(group.id) && g_groupCollapsed.at(group.id);
                    if (collapsed) continue;
                    for (const auto& request : items) {
                        if (request.groupId != group.id) continue;
                        drawRequestRow(content, groupRow + ".req." + std::to_string(request.id),
                                       contentWidth - 8.0f, indent + 14.0f, request, theme);
                    }
                    drawTree(group.id, indent + 14.0f);
                }
            };
            drawTree(0, 0.0f);
            for (const auto& request : items) {
                if (request.groupId != 0) continue;
                drawRequestRow(content, "sidebar.req." + std::to_string(request.id),
                               contentWidth - 8.0f, 0.0f, request, theme);
            }
        }).build();

    components::contextMenu(ui, "sidebar.collection.menu")
        .open(g_collectionMenuOpen).screen(screen.width, screen.height)
        .position(g_collectionMenuX, g_collectionMenuY).size(150.0f, 26.0f)
        .items({"新建请求", "新建目录"}).theme(tokens).zIndex(100)
        .onSelect([](int index) {
            g_collectionMenuOpen = false;
            if (index == 0) openRequestTypeMenu(g_collectionMenuX, g_collectionMenuY);
            else {
                g_newGroupParentId = 0;
                g_newGroupOpen = true;
            }
        }).onOpenChange([](bool open) { g_collectionMenuOpen = open; }).build();

    components::contextMenu(ui, "sidebar.group.menu")
        .open(g_groupMenuOpen).screen(screen.width, screen.height)
        .position(g_groupMenuX, g_groupMenuY).size(172.0f, 26.0f)
        .items(std::vector<components::ContextMenuItem>{
            {"新建 HTTP 请求"},
            {"新建子目录"},
            {"其他请求类型", {{"WebSocket"}, {"TCP"}}},
            {"移入当前请求"},
            {"重命名"},
            {"删除"},
        })
        .theme(tokens).zIndex(102)
        .onSelectPath([](const std::vector<int>& path) {
            const std::int64_t groupId = g_groupMenuTargetId;
            const db::Group* group = g_requests.findGroup(groupId);
            if (!group) { showStatus("目录不存在或已删除"); return; }
            const std::string name = group->name;
            if (path == std::vector<int>{0}) createHttpDraft(groupId);
            else if (path == std::vector<int>{1}) {
                g_newGroupParentId = groupId;
                g_newGroupOpen = true;
            } else if (path == std::vector<int>{2, 0}) {
                createWebSocketDraft(groupId);
            } else if (path == std::vector<int>{2, 1}) {
                createTcpDraft(api::RequestKind::Tcp, groupId);
            } else if (path == std::vector<int>{3}) {
                RequestTab& tab = activeTab();
                tab.draft.groupId = groupId;
                if (tab.requestId == 0) showStatus("新请求将保存到: " + name);
                else {
                    const std::string err = g_requests.moveToGroup(tab.requestId, groupId);
                    showStatus(err.empty() ? "已移动到: " + name : ("移动失败: " + err));
                }
            } else if (path == std::vector<int>{4}) {
                g_groupRenameText = name;
                g_renamingGroupId = groupId;
            } else if (path == std::vector<int>{5}) {
                askConfirm("删除目录", std::format("将删除目录「{}」及其子目录；其中请求将移为未分组。", name),
                           [groupId] {
                               const std::string err = g_requests.deleteGroup(groupId);
                               g_groupCollapsed.erase(groupId);
                               showStatus(err.empty() ? "目录已删除" : ("删除失败: " + err));
                           });
            }
        }).onOpenChange([](bool open) {
            g_groupMenuOpen = open;
            if (!open) g_groupMenuTargetId = 0;
        }).build();

    components::contextMenu(ui, "sidebar.request.type.menu")
        .open(g_requestTypeMenuOpen).screen(screen.width, screen.height)
        .position(g_requestTypeMenuX, g_requestTypeMenuY).size(178.0f, 26.0f)
        .items(std::vector<components::ContextMenuItem>{
            {"HTTP 请求"},
            {"其他请求类型", {{"WebSocket"}, {"TCP"}}},
        })
        .theme(tokens).zIndex(103)
        .onSelectPath([](const std::vector<int>& path) {
            const std::int64_t groupId = g_requestTypeGroupId;
            if (path == std::vector<int>{0}) createHttpDraft(groupId);
            else if (path == std::vector<int>{1, 0}) createWebSocketDraft(groupId);
            else if (path == std::vector<int>{1, 1}) createTcpDraft(api::RequestKind::Tcp, groupId);
        }).onOpenChange([](bool open) {
            g_requestTypeMenuOpen = open;
            if (!open) g_requestTypeGroupId = 0;
        }).build();

    components::contextMenu(ui, "sidebar.request.menu")
        .open(g_requestMenuOpen).screen(screen.width, screen.height)
        .position(g_requestMenuX, g_requestMenuY).size(150.0f, 26.0f)
        .items({"重命名", "删除"}).theme(tokens).zIndex(101)
        .onSelect([](int index) {
            const std::int64_t requestId = g_requestMenuTargetId;
            g_requestMenuOpen = false;
            g_requestMenuTargetId = 0;
            const db::SavedRequest* request = g_requests.find(requestId);
            if (!request) { showStatus("请求不存在或已删除"); return; }
            if (index == 0) {
                g_renameRequestId = requestId;
                g_requestRenameText = request->name;
                g_renameRequestOpen = true;
            } else {
                const std::string name = request->name;
                askConfirm("删除请求", std::format("将删除请求「{}」。", name),
                           [requestId] { deleteRequest(requestId); });
            }
        }).onOpenChange([](bool open) {
            g_requestMenuOpen = open;
            if (!open) g_requestMenuTargetId = 0;
        }).build();
}

export void drawSidebarDialogs(eui::Ui& ui, const eui::Screen& screen, const AppTheme& theme) {
    drawNewGroupDialog(ui, screen, theme);
    drawRenameRequestDialog(ui, screen, theme);
}
