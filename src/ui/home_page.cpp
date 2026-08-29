// home_page.cpp — 主页面：两栏岛屿布局。
//   左岛（固定宽）：组织列表（选中高亮），标题行加号按钮弹窗新建组织；
//   右岛（Grow）：当前组织的项目卡片（Flow 自动换行），标题行加号按钮弹窗新建项目。
// 打开项目 = selectProjectInOrg（领域状态）+ 新增/激活顶级项目标签页 + 跳转请求页；
// 未打开前，依赖项目的页面（请求集合等）一律不可用。
// 所有会触发重组卸载被点击节点的写 State / 领域变更（切组织、新建、删除、打开项目）
// 一律经 tasks.Launch + Delay(0) 推迟出指针事件路径（CLAUDE.md 约定 6）。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ui.h"

import apitab.db;
import apitab.preferences;
import apitab.store.loadtest;
import apitab.store.requests;

namespace apitab::ui {

namespace {

// 组织列表行：名称区（点击选中）与 ✕ 删除区为兄弟节点（对齐 app.cpp ProjectTab 的
// 写法），避免嵌套点击歧义；删除/切换都会重组卸载本行，故均推迟执行。
[[huxerui::composable]] huxerui::View OrgRow(const db::Org& org, bool selected,
                                             huxerui::State<int> refresh) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    return huxerui::Row {
        huxerui::Text(org.name, huxerui::TextRole::Label)
            .With(huxerui::Grow(1.0F), huxerui::Padding(4.0F),
                  huxerui::Foreground(selected ? theme.colors.on_surface
                                               : theme.colors.on_surface_variant))
            .OnClick([tasks, toast, refresh, id = org.id] {
                // 切组织会级联切项目并重载列表：重组卸载本行，推迟出指针事件路径
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    if (const std::string err = g_requests.selectOrg(id); !err.empty()) {
                        toast.Show("切换组织失败: " + err);
                        co_return;
                    }
                    refresh = refresh.Get() + 1;
                });
            }),
        huxerui::Text("✕", huxerui::TextRole::Label)
            .With(huxerui::Padding(4.0F),
                  huxerui::Foreground(theme.colors.on_surface_variant))
            .OnClick([tasks, toast, refresh, id = org.id] {
                // 删除组织级联删项目与请求，并卸载本行：推迟出指针事件路径
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    if (const std::string err = g_requests.deleteOrg(id); !err.empty()) {
                        toast.Show("删除组织失败: " + err);
                        co_return;
                    }
                    refresh = refresh.Get() + 1;
                });
            })
            .On<huxerui::ViewEvents::PointerMove>([](const huxerui::PointerEvent&) {}),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              selected ? huxerui::Background(theme.colors.surface_container_highest)
                       : huxerui::Background(huxerui::Color::Transparent()));
}

// 项目卡片：固定尺寸，Flow 内自动换行；点击打开 = 领域选择 + 标签页 + 跳请求页。
[[huxerui::composable]] huxerui::View ProjectCard(const db::Project& project,
                                                  huxerui::State<std::size_t> navPage,
                                                  huxerui::State<std::vector<std::int64_t>> tabs,
                                                  huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    const bool is_open = activeProject.Get() == project.id;
    return huxerui::Column {
        huxerui::Text(project.name, huxerui::TextRole::Title),
        huxerui::Text("ID: " + std::to_string(project.id), huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Spacer(),
        huxerui::Text(is_open ? "已打开" : "点击打开", huxerui::TextRole::Body)
            .With(huxerui::Foreground(is_open ? theme.colors.primary
                                              : theme.colors.on_surface_variant)),
    }
        .With(huxerui::Frame{.width = 200.0F, .height = 96.0F},
              huxerui::Padding(theme.spacing.medium), huxerui::Spacing(4.0F),
              huxerui::Background(theme.colors.surface_container),
              huxerui::CornerRadius(theme.shapes.medium))
        .OnClick([=] {
            // 推迟出指针事件路径：本点击会切页（卸载本卡片子树），
            // 在 pointer-up 处理中同步写 State 会触发框架段错误。
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                if (const std::string err = g_requests.selectProjectInOrg(
                        g_requests.currentOrgId(), project.id);
                    !err.empty()) {
                    toast.Show("打开失败: " + err);
                    co_return;
                }
                g_loadtest.setProject(project.id);
                saveSessionPreference("active_project", std::to_string(project.id));
                // 新增（或激活）顶级项目标签页，并持久化标签列表
                std::vector<std::int64_t> open = tabs.Get();
                if (!std::ranges::contains(open, project.id)) {
                    open.push_back(project.id);
                    std::string csv;
                    for (std::size_t i = 0; i < open.size(); ++i) {
                        csv += (i ? "," : "");
                        csv += std::to_string(open[i]);
                    }
                    saveSessionPreference("open_projects", csv);
                }
                tabs = open;
                activeProject = project.id;
                navPage = 1; // 请求页
            });
        });
}

} // namespace

[[huxerui::composable]] huxerui::View HomePage(huxerui::State<std::size_t> navPage,
                                               huxerui::State<std::vector<std::int64_t>> tabs,
                                               huxerui::State<std::int64_t> activeProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto refresh = huxerui::UseState(0);
    auto newOrgName = huxerui::UseState(huxerui::TextEditingValue{});
    auto newProjectName = huxerui::UseState(huxerui::TextEditingValue{});

    // 数据快照：组合期读领域 store（单线程 UI，重组合时重新拉取）。
    const std::int64_t refreshKey = refresh.Get();
    (void)refreshKey;
    const std::vector<db::Org> orgs = g_requests.orgs();
    const std::int64_t currentOrg = g_requests.currentOrgId();
    const std::vector<db::Project> projects = g_requests.projects();
    std::string orgName;
    for (const db::Org& org : orgs) {
        if (org.id == currentOrg) orgName = org.name;
    }

    // ---- 左岛：组织列表 + 新建组织（标题行加号按钮弹窗创建）----
    std::vector<huxerui::View> orgChildren;
    orgChildren.push_back(
        huxerui::Row {
            huxerui::Text("组织", huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
            huxerui::Button("+").OnClick([dialog, tasks, toast, refresh, newOrgName] {
                dialog.Show(
                    [tasks, toast, refresh, newOrgName](huxerui::DialogContext ctx)
                        -> huxerui::View {
                        return huxerui::Column {
                            huxerui::Text("新建组织", huxerui::TextRole::Title),
                            huxerui::TextField(newOrgName)
                                .Label("组织名称")
                                .Variant(huxerui::TextFieldVariant::Outlined)
                                .OnChanged([newOrgName](const huxerui::TextEditingValue& value) {
                                    newOrgName = value;
                                }),
                            huxerui::Row {
                                huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                                huxerui::Button("创建")
                                    .OnClick([ctx, tasks, toast, refresh, newOrgName] {
                                        if (newOrgName.Get().text.empty()) {
                                            toast.Show("组织名称不能为空");
                                            return;
                                        }
                                        ctx.Dismiss();
                                        // 新建会切到新组织并重组本页：推迟出指针事件路径
                                        tasks.Launch([=]() -> huxerui::Task<void> {
                                            co_await huxerui::Delay(
                                                std::chrono::duration<double>{0});
                                            if (const std::string err = g_requests.createOrg(
                                                    newOrgName.Get().text);
                                                !err.empty()) {
                                                toast.Show("新建组织失败: " + err);
                                                co_return;
                                            }
                                            newOrgName = huxerui::TextEditingValue{};
                                            refresh = refresh.Get() + 1;
                                        });
                                    }),
                            }
                                .With(huxerui::Spacing(8.0F)),
                        }
                            .With(huxerui::Padding(20.0F), huxerui::Spacing(12.0F),
                                  huxerui::Frame{.width = 320.0F});
                    },
                    huxerui::DialogOptions{});
            }),
        }
            .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    if (orgs.empty()) {
        orgChildren.push_back(huxerui::Text("暂无组织", huxerui::TextRole::Body)
                                  .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }
    for (const db::Org& org : orgs) {
        orgChildren.push_back(OrgRow(org, org.id == currentOrg, refresh).Key(org.id));
    }
    huxerui::View orgIsland =
        huxerui::Column(std::move(orgChildren))
            .With(huxerui::Padding(theme.spacing.medium),
                  huxerui::Spacing(theme.spacing.small),
                  huxerui::Background(theme.colors.surface_container_low),
                  huxerui::CornerRadius(theme.shapes.large), huxerui::Frame{.width = 240.0F},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // ---- 右岛：当前组织的项目卡片 + 新建项目 ----
    std::vector<huxerui::View> cards;
    for (const db::Project& project : projects) {
        cards.push_back(ProjectCard(project, navPage, tabs, activeProject).Key(project.id));
    }
    huxerui::View projectIsland =
        huxerui::Column {
            huxerui::Row {
                PageHeader("项目",
                           orgName.empty() ? "当前组织的项目" : "当前组织：" + orgName)
                    .With(huxerui::Grow(1.0F)),
                huxerui::Button("+").OnClick([dialog, tasks, toast, refresh, newProjectName] {
                    dialog.Show(
                        [tasks, toast, refresh, newProjectName](huxerui::DialogContext ctx)
                            -> huxerui::View {
                            return huxerui::Column {
                                huxerui::Text("新建项目", huxerui::TextRole::Title),
                                huxerui::TextField(newProjectName)
                                    .Label("项目名称")
                                    .Variant(huxerui::TextFieldVariant::Outlined)
                                    .OnChanged(
                                        [newProjectName](const huxerui::TextEditingValue& value) {
                                            newProjectName = value;
                                        }),
                                huxerui::Row {
                                    huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                                    huxerui::Button("创建")
                                        .OnClick([ctx, tasks, toast, refresh, newProjectName] {
                                            if (newProjectName.Get().text.empty()) {
                                                toast.Show("项目名称不能为空");
                                                return;
                                            }
                                            ctx.Dismiss();
                                            // 新建会重载项目列表并重组本页：推迟出指针事件路径
                                            tasks.Launch([=]() -> huxerui::Task<void> {
                                                co_await huxerui::Delay(
                                                    std::chrono::duration<double>{0});
                                                if (const std::string err =
                                                        g_requests.createProject(
                                                            newProjectName.Get().text);
                                                    !err.empty()) {
                                                    toast.Show("新建项目失败: " + err);
                                                    co_return;
                                                }
                                                newProjectName = huxerui::TextEditingValue{};
                                                refresh = refresh.Get() + 1;
                                            });
                                        }),
                                }
                                    .With(huxerui::Spacing(8.0F)),
                            }
                                .With(huxerui::Padding(20.0F), huxerui::Spacing(12.0F),
                                      huxerui::Frame{.width = 320.0F});
                        },
                        huxerui::DialogOptions{});
                }),
            }
                .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            projects.empty()
                ? huxerui::View{huxerui::Text("该组织暂无项目，点击右上角 + 新建一个。",
                                              huxerui::TextRole::Body)
                                    .With(huxerui::Foreground(theme.colors.on_surface_variant))}
                : huxerui::View{huxerui::Flow(std::move(cards))
                                    .With(huxerui::Spacing(theme.spacing.medium))},
        }
            .With(huxerui::Padding(theme.spacing.large),
                  huxerui::Spacing(theme.spacing.medium),
                  huxerui::Background(theme.colors.surface_container_low),
                  huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    return huxerui::ScrollView {
        huxerui::Row {
            orgIsland,
            projectIsland,
        }.With(huxerui::Spacing(theme.spacing.medium),
               huxerui::Padding(theme.spacing.large)),
    }.With(huxerui::ScrollBar());
}

} // namespace apitab::ui
