// home_page.cpp — 主页面（顶级 Home 标签内容，全宽无侧栏）：两栏岛屿布局。
//   左岛（固定宽）：组织列表（选中高亮），标题行加号按钮弹窗新建组织；
//   右岛（Grow）：当前组织的项目卡片（Flow 自动换行），标题行加号按钮弹窗新建项目。
// 打开项目 = selectProjectInOrg（领域状态）+ onOpenProject 回调（AppRoot 注入：
// 新增/激活顶级项目标签 + tabs/activeTopTab/lastProjectTab 写回 + open_projects
// 按需持久化，见 app.cpp）；未打开前，依赖项目的页面（请求集合等）一律不可用。
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

// 组织列表行（P1-A2 布局契约：[前置区] [主内容 Grow] [尾部信息] [固定动作区]）：
// 组织行无前置图标与尾部元信息——主内容 = 组织名（左对齐、Grow 撑开），尾部 =
// TrailingActionGroup 固定动作区（32pt 槽位整列等宽，窗口宽度变化时右缘不抖动）。
// OnClick 挂在整行 Row 上（框架点击不冒泡，最深绑定生效，动作区内 ✕ 的点击
// 仍只触发 ✕，行选择与删除是独立事件目标）。悬停反馈走 Hover 事件通道（非独占，
// 悬停 ✕ 也触发）驱动整行底色；行自身压掉默认 Indication 避免双层叠加，✕ 保留
// 自己的高亮。子节点按声明顺序即焦点序（文本 → 动作区），无打乱顺序的包装。
// 删除/切换都会重组卸载本行，故均推迟执行。
[[huxerui::composable]] huxerui::View OrgRow(const db::Org& org, bool selected,
                                             huxerui::State<int> refresh) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    auto hovered = huxerui::UseState(false);
    return huxerui::Row {
        // 主内容：组织名，左对齐 Grow 撑开，把固定动作区推到行右缘。
        huxerui::Text(org.name, huxerui::TextRole::Label)
            .With(huxerui::Grow(1.0F), huxerui::Padding(4.0F),
                  huxerui::Foreground(selected ? theme.colors.on_surface
                                               : theme.colors.on_surface_variant)),
        // 固定动作区：删除组织 ✕（Bare 28pt，保留自身 hover/press 高亮）。
        TrailingActionGroup({AppIconButton("✕", "删除组织", [tasks, toast, refresh, id = org.id] {
                // 删除组织级联删项目与请求，并卸载本行：推迟出指针事件路径
                tasks.Launch([=]() -> huxerui::Task<void> {
                    co_await huxerui::Delay(std::chrono::duration<double>{0});
                    if (const std::string err = g_requests.deleteOrg(id); !err.empty()) {
                        toast.Show("删除组织失败: " + err);
                        co_return;
                    }
                    refresh = refresh.Get() + 1;
                });
            }, AppIconButtonShape::Bare)}),
    }
        .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 2.0F)),
              huxerui::CornerRadius(theme.shapes.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              selected ? huxerui::Background(theme.colors.surface_container_highest)
                       : hovered.Get()
                             ? huxerui::Background(theme.colors.surface_container)
                             : huxerui::Background(huxerui::Color::Transparent()),
              huxerui::Indication{})
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
        })
        .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& e) {
            if (e.type == huxerui::HoverEventType::Enter)
                hovered = true;
            else if (e.type == huxerui::HoverEventType::Leave)
                hovered = false;
        });
}

// 项目卡片：固定尺寸，Flow 内自动换行；点击打开 = 领域选择 + onOpenProject
//（AppRoot：顶级项目标签新增/激活）。已打开的项目用主色名称标记（is_open 用
// activeProject 领域打开态判定，原 ID/「点击打开」提示已按设计要求移除）。
[[huxerui::composable]] huxerui::View ProjectCard(
    const db::Project& project, huxerui::State<std::int64_t> activeProject,
    const std::function<void(std::int64_t)>& onOpenProject) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    const bool is_open = activeProject.Get() == project.id;
    return huxerui::Column {
        huxerui::Text(project.name, huxerui::TextRole::Title)
            .With(huxerui::Foreground(is_open ? theme.colors.primary
                                              : theme.colors.on_surface)),
    }
        .With(huxerui::Frame{.width = 200.0F, .height = 96.0F},
              huxerui::Padding(theme.spacing.medium), huxerui::Spacing(4.0F),
              huxerui::Background(theme.colors.surface_container),
              huxerui::CornerRadius(theme.shapes.medium),
              // 键盘/语义（P1-B0.4，§13.6 键盘要求）：卡片可聚焦 + Button 语义，
              // 键盘 Tab 后 Enter/Space 打开项目（模式同 settings_page 左分类行）。
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                 .label = "打开项目 " + project.name})
        .OnClick([=] {
            // 推迟出指针事件路径：本点击会切顶级标签（卸载本卡片子树），
            // 在 pointer-up 处理中同步写 State 会触发框架段错误。
            tasks.Launch([=]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
                // 领域写入先行（§13.2 不变量 1：领域同步完成后才渲染项目工作区）。
                if (const std::string err = g_requests.selectProjectInOrg(
                        g_requests.currentOrgId(), project.id);
                    !err.empty()) {
                    toast.Show("打开失败: " + err);
                    co_return;
                }
                g_loadtest.setProject(project.id);
                saveSessionPreference("active_project", std::to_string(project.id));
                // 顶级标签新增/激活 + State 写回 + open_projects 按需持久化由
                // AppRoot 的 onOpenProject 完成；同一推迟任务内执行，重组无中间帧。
                onOpenProject(project.id);
            });
        });
}

} // namespace

[[huxerui::composable]] huxerui::View HomePage(
    std::function<void(std::int64_t)> onOpenProject,
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
    // 列表内部滚动（外岛固定尺寸，参照请求页左岛）。
    std::vector<huxerui::View> orgRows;
    if (orgs.empty()) {
        orgRows.push_back(huxerui::Text("暂无组织", huxerui::TextRole::Body)
                              .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }
    for (const db::Org& org : orgs) {
        orgRows.push_back(OrgRow(org, org.id == currentOrg, refresh).Key(org.id));
    }
    huxerui::View orgIsland =
        huxerui::Column {
            huxerui::Row {
                huxerui::Text("组织", huxerui::TextRole::Title).With(huxerui::Grow(1.0F)),
                // 独立浮动 + 动作：圆形 + 主色底（28pt 命中区），语义标签"新建组织"。
                AppIconButton("+", "新建组织", [dialog, tasks, toast, refresh, newOrgName] {
                dialog.Show(
                    [tasks, toast, refresh, newOrgName](huxerui::DialogContext ctx)
                        -> huxerui::View {
                        return DialogCard(huxerui::Column {
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
                                // 两端对齐：取消在左、创建在右；内容列
                                // CrossAlign(Stretch) 把按钮行拉到卡片整宽。
                                .With(huxerui::Spacing(8.0F),
                                      huxerui::MainAlign(
                                          huxerui::MainAxisAlignment::SpaceBetween)),
                        }
                                            .With(huxerui::Spacing(12.0F),
                                                  huxerui::Frame{.width = 320.0F},
                                                  huxerui::CrossAlign(
                                                      huxerui::CrossAxisAlignment::Stretch)));
                    },
                    huxerui::DialogOptions{});
            }, AppIconButtonShape::Circular, 28.0F, true),
            }
                .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::ScrollView{huxerui::Column(std::move(orgRows))
                                    .With(huxerui::Spacing(theme.spacing.small))}
                .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
        }
            .With(huxerui::Padding(theme.spacing.medium),
                  huxerui::Spacing(theme.spacing.small),
                  huxerui::Background(theme.colors.surface_container_low),
                  huxerui::CornerRadius(theme.shapes.large), huxerui::Frame{.width = 240.0F},
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // ---- 右岛：当前组织的项目卡片 + 新建项目 ----
    std::vector<huxerui::View> cards;
    for (const db::Project& project : projects) {
        cards.push_back(ProjectCard(project, activeProject, onOpenProject).Key(project.id));
    }
    huxerui::View projectIsland =
        huxerui::Column {
            huxerui::Row {
                PageHeader("项目",
                           orgName.empty() ? "当前组织的项目" : "当前组织：" + orgName)
                    .With(huxerui::Grow(1.0F)),
                // 独立浮动 + 动作：圆形 + 主色底（28pt 命中区），语义标签"新建项目"。
                AppIconButton("+", "新建项目", [dialog, tasks, toast, refresh, newProjectName] {
                    dialog.Show(
                        [tasks, toast, refresh, newProjectName](huxerui::DialogContext ctx)
                            -> huxerui::View {
                            return DialogCard(huxerui::Column {
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
                                    // 两端对齐：取消在左、创建在右；内容列
                                    // CrossAlign(Stretch) 把按钮行拉到卡片整宽。
                                    .With(huxerui::Spacing(8.0F),
                                          huxerui::MainAlign(
                                              huxerui::MainAxisAlignment::SpaceBetween)),
                            }
                                                .With(huxerui::Spacing(12.0F),
                                                      huxerui::Frame{.width = 320.0F},
                                                      huxerui::CrossAlign(
                                                          huxerui::CrossAxisAlignment::Stretch)));
                        },
                        huxerui::DialogOptions{});
                }, AppIconButtonShape::Circular, 28.0F, true),
            }
                .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
            huxerui::ScrollView{
                projects.empty()
                    ? huxerui::View{huxerui::Text("该组织暂无项目，点击右上角 + 新建一个。",
                                                  huxerui::TextRole::Body)
                                        .With(huxerui::Foreground(theme.colors.on_surface_variant))}
                    : huxerui::View{huxerui::Flow(std::move(cards))
                                        .With(huxerui::Spacing(theme.spacing.medium))}}
                .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
        }
            .With(huxerui::Padding(theme.spacing.large),
                  huxerui::Spacing(theme.spacing.medium),
                  huxerui::Background(theme.colors.surface_container_low),
                  huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));

    // 根 Row 撑满窗口（Grow）：组织岛定宽在左、项目岛 Grow 在右，整体左对齐。
    // 不用外层 ScrollView（其内容宽度无界会让 Row 收缩漂移），两岛各自内部滚动。
    return huxerui::Row {
        std::move(orgIsland),
        std::move(projectIsland),
    }
        .With(huxerui::Spacing(theme.spacing.small), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
