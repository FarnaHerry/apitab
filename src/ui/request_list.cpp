// request_list.cpp — 请求工作区左岛：当前项目请求树（分组按 parentId 折叠/叶子、
// 行尾 ⋮ / 右键统一菜单、拖拽移入分组或根目录）+ 头部“+”新建与导入菜单入口。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 请求集合树），行为保持、UI 无改动。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "draft.h"
#include "ui.h"

import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.utils;

namespace apitab::ui {

namespace {
// ---- 集合树拆分带出的草稿 ⇄ 落库桥接（db::SavedRequest → RequestDraft）----

// api::KeyValue → 行模型。与 request_page.cpp（环境变量表单）同逻辑；两处都是
// TU 内实现——签名含模块类型 api::KeyValue，普通头无法声明（CLAUDE.md 模块
// 约束）。P1-C3 整理公共声明时统一唯一桥接实现。
KvRow FromKeyValue(const api::KeyValue& kv) {
    return KvRow{.key = huxerui::TextEditingValue{kv.key},
                 .value = huxerui::TextEditingValue{kv.value},
                 .type = huxerui::TextEditingValue{kv.type},
                 .remark = huxerui::TextEditingValue{kv.remark},
                 .enabled = kv.enabled};
}

TestCaseDraft CaseFromDb(const db::RequestTestCase& c) {
    TestCaseDraft out;
    out.name = huxerui::TextEditingValue{c.name};
    out.enabled = c.enabled;
    if (c.expectStatus != 0) out.expectStatus = huxerui::TextEditingValue{std::to_string(c.expectStatus)};
    if (c.maxMs > 0.0) out.maxMs = huxerui::TextEditingValue{std::to_string(static_cast<long long>(c.maxMs))};
    for (const db::RequestAssertion& a : c.asserts) {
        out.asserts.push_back(KvRow{.key = huxerui::TextEditingValue{a.path},
                                    .value = huxerui::TextEditingValue{a.equals},
                                    .enabled = a.enabled});
    }
    return out;
}

MockDraft MockFromDb(const db::RequestMock& m) {
    MockDraft out;
    out.enabled = m.enabled;
    out.status = huxerui::TextEditingValue{std::to_string(m.status)};
    out.delayMs = huxerui::TextEditingValue{std::to_string(m.delayMs)};
    out.body = huxerui::TextEditingValue{m.body};
    for (const api::KeyValue& kv : m.headers) out.headers.push_back(FromKeyValue(kv));
    return out;
}

RequestDraft DraftFromSaved(const db::SavedRequest& saved) {
    RequestDraft draft;
    draft.savedId = saved.id;
    draft.kind = static_cast<int>(saved.kind);
    if (draft.kind < 0 || draft.kind > 2) draft.kind = 0; // 防御：未知类型按 HTTP 打开
    draft.name = huxerui::TextEditingValue{saved.name};
    for (std::size_t i = 0; i < kMethodNames.size(); ++i) {
        if (kMethodNames[i] == saved.method) draft.methodIndex = i;
    }
    draft.url = huxerui::TextEditingValue{saved.url};
    for (const api::KeyValue& kv : saved.params) draft.params.push_back(FromKeyValue(kv));
    for (const api::KeyValue& kv : saved.headers) draft.headers.push_back(FromKeyValue(kv));
    for (const api::KeyValue& kv : saved.cookies) draft.cookies.push_back(FromKeyValue(kv));
    draft.bodyKindIndex = static_cast<std::size_t>(saved.bodyKind);
    if (draft.bodyKindIndex >= kBodyTypeNames.size()) draft.bodyKindIndex = 0;
    // 各文本类 body 独立存档：bodyContents 全量载入各类型槽位，切换类型
    // 互不影响；兼容字段 body 回落到当前类型槽位。fields 只取当前类型的。
    for (std::size_t i = 0; i < saved.bodyContents.size(); ++i) {
        if (!saved.bodyContents[i].text.empty())
            draft.bodies[i] = huxerui::TextEditingValue{saved.bodyContents[i].text};
    }
    if (draft.bodies[draft.bodyKindIndex].text.empty())
        draft.bodies[draft.bodyKindIndex] = huxerui::TextEditingValue{saved.body};
    if (draft.bodyKindIndex < saved.bodyContents.size()) {
        for (const api::KeyValue& kv : saved.bodyContents[draft.bodyKindIndex].fields)
            draft.bodyFields.push_back(FromKeyValue(kv));
    }
    for (const db::RequestTestCase& c : saved.testCases) draft.cases.push_back(CaseFromDb(c));
    draft.mock = MockFromDb(saved.mock);
    return draft;
}

// 拖拽载荷：请求行 / 分组行（typed payload，DragSource/DropTarget 按类型匹配）。
struct RequestDragPayload {
    std::int64_t requestId = 0;
};
struct GroupDragPayload {
    std::int64_t groupId = 0;
};

// 行尾 ⋮ 菜单按钮：常驻、默认透明（Opacity 是 paint 修饰符，悬停显隐只改绘制、
// 不换子节点类型/数量——避免悬停重组时子树卸载重建引发 hover 振荡抖动）；
// 透明时点击空转。行悬停态由行最外层容器的 Hover 事件维护（按钮在行边界内，
// 悬停 ⋮ 不退出行的悬停态），本按钮不再单独跟踪。必须是独立 composable：
// 菜单锚点（LayerAnchor）只能挂载在一个 View 上（"presentation anchor must be
// mounted on only one View"），每个按钮实例需要在自己的作用域里 UsePopup 拿独立锚点。
// 菜单用自绘 PopupMenu（危险项 hover 才显红）。
[[huxerui::composable]] huxerui::View RowMenuButton(bool visible,
                                                    std::vector<AppMenuItem> items) {
    auto popup = huxerui::UsePopup();
    // OverflowButton：AppIconButton("⋮", "更多操作", Bare, 28) 的语义封装，第 2 参
    // = enabled（原第 7 参同义）——不可见时点击空转；菜单内容/弹出行为零变化。
    return OverflowButton([popup, items = std::move(items)] {
              ShowAppMenu(popup, std::move(items),
                          huxerui::PopupOptions{
                              .placement = {huxerui::AnchorSide::Below,
                                            huxerui::AnchorAlignment::Start}});
          }, visible)
        .With(popup.Anchor(), huxerui::Opacity(visible ? 1.0F : 0.0F));
}
} // namespace

// 左岛：当前项目的请求树（分组按 parentId 层级渲染为可折叠节点，请求为叶子，
// 点击开标签，行尾 ⋮ / 右键菜单做重命名与删除）+ 头部圆形 "+" 新建类型菜单
// （自绘 PopupMenu，条目分隔线分组）。
// vertical=true 用于 Compact 视口：列表改为顶部横岛（限高、宽度撑满）。
[[huxerui::composable]] huxerui::View RequestListIsland(
    huxerui::State<std::vector<RequestDraft>> drafts, huxerui::State<std::size_t> activeTab,
    huxerui::State<int> listVersion, bool vertical) {

    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto addPopup = huxerui::UsePopup(); // "+" 新建菜单（自绘 PopupMenu，卡片观感统一）
    auto ctxMenu = huxerui::UsePopup(); // 行右键菜单（自绘 PopupMenu）：ShowAt 无需锚点，所有行共享
    auto dialog = huxerui::UseDialog();
    auto toast = huxerui::UseToast();
    (void)listVersion.Get(); // 订阅列表版本：保存/删除后触发本岛重组

    // 新建接口目录弹窗的受控状态（"+" 菜单 → 新建接口目录… 时重置）。
    auto newGroupName = huxerui::UseState(huxerui::TextEditingValue{});
    auto newGroupPath = huxerui::UseState(huxerui::TextEditingValue{});

    // 悬停行：请求行 = 请求 id，分组行 = -分组 id（两表 id 空间会撞，用符号区分），
    // 0 = 无。Hover 事件是包含生命周期：进入行呈现边界发 Enter、离开才发 Leave，
    // 行内子组件（打开区/⋮ 按钮）之间移动不重触发——挂在行最外层即覆盖整行。
    auto hoveredRow = huxerui::UseState<std::int64_t>(0);
    // 重命名弹窗的输入值（打开前预填当前名称）。
    auto renameValue = huxerui::UseState(huxerui::TextEditingValue{});
    // 接口目录编辑是名称 + 路径双字段；不能再复用上面的单字段请求重命名状态。
    auto groupNameValue = huxerui::UseState(huxerui::TextEditingValue{});
    auto groupPathValue = huxerui::UseState(huxerui::TextEditingValue{});

    const auto methodFont =
        huxerui::Font::Monospace(font_size::kCaption).WithWeight(huxerui::FontWeight::SemiBold);

    // 折叠状态：收起的分组 id 集（默认全部展开）。切换只重组本岛，
    // 不卸载被点的分组行本身，同步写即可。
    auto collapsed = huxerui::UseState<std::vector<std::int64_t>>({});

    std::vector<huxerui::View> rows;
    const std::vector<db::SavedRequest>& saved = g_requests.list();
    const std::vector<db::Group>& groups = g_requests.groups();

    // 行底色只给"被选中"的条目——当前活跃标签对应的集合项；其余行透明。
    const std::vector<RequestDraft> openTabs = drafts.Get();
    const std::int64_t activeSavedId =
        activeTab.Get() < openTabs.size() ? openTabs[activeTab.Get()].savedId : -1;

    // 重命名弹窗（请求/分组共用）：预填当前名，apply 回调落库并返回错误串
    // （空 = 成功），成功后 bump listVersion 重组本岛。布局同「新建分组」弹窗。
    auto showRenameDialog =
        [dialog, tasks, toast, listVersion, renameValue](
            const std::string& currentName,
            const std::function<std::string(const std::string&)>& apply) {
            renameValue = huxerui::TextEditingValue{currentName};
            dialog.Show(
                [tasks, toast, listVersion, renameValue,
                 apply](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("重命名", huxerui::TextRole::Title),
                        huxerui::TextField(renameValue.Get())
                            .Label("名称")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([renameValue](const huxerui::TextEditingValue& value) {
                                renameValue = value;
                            }),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                            huxerui::Button("确定")
                                .OnClick([ctx, tasks, toast, listVersion, renameValue, apply] {
                                    const std::string name = renameValue.Get().text;
                                    if (name.empty()) {
                                        toast.Show("名称不能为空");
                                        return;
                                    }
                                    ctx.Dismiss();
                                    // 落库后重组左岛：推迟出指针事件路径
                                    tasks.Launch([=]() -> huxerui::Task<void> {
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        if (const std::string err = apply(name); !err.empty()) {
                                            toast.Show("重命名失败: " + err);
                                            co_return;
                                        }
                                        listVersion = listVersion.Get() + 1;
                                    });
                                }),
                        }
                            .With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
                    }
                        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{});
        };

    // 接口目录编辑：与“新建接口目录”保持同一名称/路径契约。路径为空即仅名称目录，
    // 非空即 Path 目录；一次提交原子更新 name/mode/path，避免 URL 仍拼旧路径。
    auto showGroupEditDialog =
        [dialog, tasks, toast, listVersion, groupNameValue,
         groupPathValue](const db::Group& group) {
            groupNameValue = huxerui::TextEditingValue{group.name};
            groupPathValue = huxerui::TextEditingValue{group.path};
            const std::int64_t gid = group.id;
            dialog.Show(
                [tasks, toast, listVersion, groupNameValue, groupPathValue,
                 gid](huxerui::DialogContext ctx) -> huxerui::View {
                    return DialogCard(huxerui::Column {
                        huxerui::Text("编辑接口目录", huxerui::TextRole::Title),
                        huxerui::TextField(groupNameValue.Get())
                            .Label("名称")
                            .Placeholder("接口目录名称")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([groupNameValue](const huxerui::TextEditingValue& value) {
                                groupNameValue = value;
                            }),
                        huxerui::TextField(groupPathValue.Get())
                            .Label("路径")
                            .Placeholder("api/v1（留空则仅作名称）")
                            .Variant(huxerui::TextFieldVariant::Outlined)
                            .OnChanged([groupPathValue](const huxerui::TextEditingValue& value) {
                                groupPathValue = value;
                            }),
                        huxerui::Row {
                            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
                            huxerui::Button("保存").OnClick(
                                [ctx, tasks, toast, listVersion, groupNameValue,
                                 groupPathValue, gid] {
                                    const std::string name = trim(groupNameValue.Get().text);
                                    const std::string path = trim(groupPathValue.Get().text);
                                    if (name.empty()) {
                                        toast.Show("名称不能为空");
                                        return;
                                    }
                                    ctx.Dismiss();
                                    tasks.Launch([=]() -> huxerui::Task<void> {
                                        co_await huxerui::Delay(
                                            std::chrono::duration<double>{0});
                                        if (const std::string err =
                                                g_requests.updateGroup(gid, name, path);
                                            !err.empty()) {
                                            toast.Show("保存接口目录失败: " + err);
                                            co_return;
                                        }
                                        toast.Show("接口目录已更新");
                                        listVersion = listVersion.Get() + 1;
                                    });
                                }),
                        }.With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
                    }.With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
                           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
                },
                huxerui::DialogOptions{});
        };

    // 请求行的菜单条目（重命名/删除）：行尾 ⋮ 按钮与右键菜单共用，
    // 按行现场构造，避免两处逻辑分叉。自绘 PopupMenu：删除项 hover 才显红。
    auto requestEntries = [showRenameDialog, dialog, tasks, drafts, activeTab,
                           listVersion](const db::SavedRequest& r) {
        const std::int64_t id = r.id;
        return std::vector<AppMenuItem>{
            AppMenuItem{
                .label = "重命名",
                .onClick = [showRenameDialog, drafts, id, currentName = r.name] {
                    // 重命名后同步已打开标签的名字。
                    showRenameDialog(currentName, [drafts, id](const std::string& name) {
                        const std::string err = g_requests.renameRequest(id, name);
                        if (err.empty()) {
                            std::vector<RequestDraft> copy = drafts.Get();
                            for (RequestDraft& d : copy)
                                if (d.savedId == id)
                                    d.name = huxerui::TextEditingValue{name};
                            drafts = copy;
                        }
                        return err;
                    });
                }},
            AppMenuItem{
                .label = "删除",
                .onClick = [dialog, tasks, drafts, activeTab, listVersion, id,
                            name = r.name] {
                    // 删除前先弹危险确认框（确认按钮染红）；真正删行仍在确认回调里
                    // 推迟出指针事件路径。
                    ShowDangerConfirm(dialog, "删除请求",
                                      "确定删除请求「" + (name.empty() ? "未命名" : name) +
                                          "」吗？此操作不可恢复。",
                                      "删除", [tasks, drafts, activeTab, listVersion, id] {
                                          tasks.Launch([=]() -> huxerui::Task<void> {
                                              co_await huxerui::Delay(
                                                  std::chrono::duration<double>{0});
                                              (void)g_requests.remove(id);
                                              std::vector<RequestDraft> copy = drafts.Get();
                                              for (std::size_t i = 0; i < copy.size(); ++i) {
                                                  if (copy[i].savedId == id) {
                                                      copy.erase(copy.begin() +
                                                                 static_cast<long>(i));
                                                      if (!copy.empty() &&
                                                          activeTab.Get() >= copy.size())
                                                          activeTab = copy.size() - 1;
                                                      break;
                                                  }
                                              }
                                              drafts = copy;
                                              listVersion = listVersion.Get() + 1;
                                          });
                                      });
                },
                .tone = AppMenuTone::DangerHover}};
    };

    // 接口目录菜单：行尾 ⋮ 与右键共用同一条目模型和双字段编辑流程。
    auto groupEntries = [showGroupEditDialog, tasks, toast, listVersion](const db::Group& g) {
        const std::int64_t gid = g.id;
        return std::vector<AppMenuItem>{
            AppMenuItem{
                .label = "编辑接口目录",
                .onClick = [showGroupEditDialog, g] { showGroupEditDialog(g); }},
            AppMenuItem{
                .label = "删除接口目录",
                .onClick = [tasks, toast, listVersion, gid] {
                    // 删除接口目录会重组本岛：推迟出指针事件路径
                    // （组内请求移到未分组，子分组一并删除）。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        if (const std::string err = g_requests.deleteGroup(gid); !err.empty()) {
                            toast.Show("删除接口目录失败: " + err);
                            co_return;
                        }
                        listVersion = listVersion.Get() + 1;
                    });
                },
                .tone = AppMenuTone::DangerHover}};
    };

    // 请求行（叶子）：徽标 + 名称 + 行尾 ⋮ 菜单（重命名/删除）；depth 只影响左侧缩进。
    auto requestRow = [&](const db::SavedRequest& r, int depth) -> huxerui::View {
        const std::int64_t id = r.id;
        // 徽标：非 HTTP 的已保存请求显示类型缩写（防御；现存数据基本都是 HTTP）。
        const std::string badge = r.kind == api::RequestKind::WebSocket ? "WS"
                                  : r.kind == api::RequestKind::Tcp     ? "TCP"
                                                                        : r.method;
        return huxerui::Row {
            // 打开区：徽标 + 名称占满行宽；点击挂在整行 Row 上（见下方 .OnClick），
            // ⋮ 是最深命中节点、点击不冒泡，仍只触发它自己。
            huxerui::Row {
                huxerui::Text(badge, huxerui::TextRole::Label)
                    .Style(huxerui::TextStyle{
                        .font = methodFont,
                        // 徽标按 MethodColor 统一色表逐方法着色。
                        .foreground = MethodColor(theme, badge)})
                    .With(huxerui::Frame{.min_width = 32.0F}),
                huxerui::Text(r.name.empty() ? "（未命名）" : r.name,
                              huxerui::TextRole::Body),
            }
                .With(huxerui::Spacing(theme.spacing.extra_small),
                      huxerui::Grow(1.0F), huxerui::ClipChildren()),
            // 行尾 ⋮ 菜单（悬停显隐；重命名/删除）。锚点在按钮自己的 composable
            // 作用域里（一个 LayerAnchor 只能挂一个 View，见 RowMenuButton）。
            RowMenuButton(hoveredRow.Get() == id, requestEntries(r)),
        }
            .With(huxerui::Spacing(0.0F),
                  huxerui::Padding(huxerui::EdgeInsets{
                      .top = 4.0F, .right = 6.0F, .bottom = 4.0F,
                      .left = 6.0F + static_cast<float>(depth) * 14.0F}),
                  // 默认无底色，被选中（活跃标签对应行）或悬停（含悬停 ⋮，
                  // Hover 事件通道非独占）才显示容器底。
                  huxerui::Background(id == activeSavedId || hoveredRow.Get() == id
                                          ? theme.colors.surface_container
                                          : huxerui::Color::Transparent()),
                  huxerui::CornerRadius(theme.shapes.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  // 行自身压掉默认 Indication：悬停反馈由手工底色承担，避免叠加。
                  huxerui::Indication{})
            // 点击整行 = 打开/激活对应标签：挂整行让默认 Indication 的悬停/按压
            // 高亮覆盖整条长条（语义同分组行）。
            .OnClick([drafts, activeTab, id] {
                // 已打开则激活，否则新建标签；左岛不卸载，同步写即可。
                std::vector<RequestDraft> copy = drafts.Get();
                for (std::size_t i = 0; i < copy.size(); ++i) {
                    if (copy[i].savedId == id) {
                        activeTab = i;
                        return;
                    }
                }
                if (const db::SavedRequest* found = g_requests.find(id)) {
                    copy.push_back(DraftFromSaved(*found));
                    drafts = copy;
                    activeTab = copy.size() - 1;
                }
            })
            // 拖拽源：鼠标按下即拖（默认距离阈值），与点击按阈值分胜负——桌面端
            // 滚动走滚轮，ScrollView 不与鼠标拖拽抢指针。preview 工厂给出拖影：
            // 拖到分组行 = 移入该分组；拖到列表空白 = 移出分组（根投放区见下）。
            .With(huxerui::DragSource(
                RequestDragPayload{id},
                [badge, name = r.name, bg = theme.colors.surface_container_high,
                 fg = theme.colors.on_surface,
                 accent = MethodColor(theme, badge)] {
                    return huxerui::Row {
                        huxerui::Text(badge, huxerui::TextRole::Label)
                            .Style(huxerui::TextStyle{
                                .font = huxerui::Font::Monospace(font_size::kCaption)
                                            .WithWeight(huxerui::FontWeight::SemiBold),
                                .foreground = accent}),
                        huxerui::Text(name.empty() ? "（未命名）" : name,
                                      huxerui::TextRole::Body),
                    }
                        .With(huxerui::Spacing(6.0F), huxerui::Padding(6.0F),
                              huxerui::Background(bg), huxerui::Foreground(fg),
                              huxerui::CornerRadius(6.0F));
                }))
            // 悬停显隐 ⋮：Enter 记行 key，Leave 时仅当仍是本行才清空（防跨行
            // 误清）。只写 hoveredRow；悬停重组靠稳定 Key 保留挂载节点（见下）。
            .On<huxerui::ViewEvents::Hover>([hoveredRow, id](const huxerui::HoverEvent& e) {
                if (e.type == huxerui::HoverEventType::Enter)
                    hoveredRow = id;
                else if (e.type == huxerui::HoverEventType::Leave && hoveredRow.Get() == id)
                    hoveredRow = 0;
            })
            // 右键菜单：条目同 ⋮ 按钮，跟随点击位置弹出；挂在行最外层容器上，
            // 命中链最深绑定生效，分组内嵌套的请求行仍弹本菜单。
            .On<huxerui::ViewEvents::ContextMenuRequested>(
                [ctxMenu, requestEntries, r](huxerui::Point pos) {
                    ShowAppMenuAt(ctxMenu, pos, requestEntries(r));
                })
            // 稳定 Key：悬停重组（整张列表重建 rows）时按 Key 保留挂载节点
            // 与其扩展实例，避免节点替换引起的 hover 抖动。
            .Key(id);
    };

    // 接口目录行（内部节点）：折叠箭头 + 名称；点击切换折叠。行尾 ⋮（悬停才显示）
    // 与右键共享“编辑接口目录 / 删除接口目录”统一菜单。
    auto groupRow = [&](const db::Group& g, int depth, bool isCollapsed) -> huxerui::View {
        return huxerui::Row {
                   huxerui::Text(isCollapsed ? "▸" : "▾", huxerui::TextRole::Label)
                       .Style(huxerui::TextStyle{
                           .font = huxerui::Font::System(font_size::kCaption),
                           .foreground = theme.colors.on_surface_variant})
                       .With(huxerui::Frame{.min_width = 14.0F}),
                   huxerui::Text(g.name, huxerui::TextRole::Body)
                       .With(huxerui::ClipChildren(), huxerui::Grow(1.0F)),
                   // 行尾 ⋮ 菜单（悬停显隐；编辑/删除接口目录）。锚点在按钮自己的
                   // composable 作用域里（一个 LayerAnchor 只能挂一个 View）。
                   RowMenuButton(hoveredRow.Get() == -g.id, groupEntries(g)),
               }
                   .With(huxerui::Spacing(theme.spacing.extra_small),
                         huxerui::Padding(huxerui::EdgeInsets{
                             .top = 4.0F, .right = 6.0F, .bottom = 4.0F,
                             .left = 6.0F + static_cast<float>(depth) * 14.0F}),
                         // 悬停（含悬停 ⋮，同请求行）显示容器底。
                         huxerui::Background(hoveredRow.Get() == -g.id
                                                 ? theme.colors.surface_container
                                                 : huxerui::Color::Transparent()),
                         huxerui::CornerRadius(theme.shapes.small),
                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                         // 行自身压掉默认 Indication：悬停反馈由手工底色承担。
                         huxerui::Indication{})
                   .OnClick([collapsed, id = g.id] {
                       std::vector<std::int64_t> copy = collapsed.Get();
                       if (const auto it = std::ranges::find(copy, id); it != copy.end())
                           copy.erase(it);
                       else
                           copy.push_back(id);
                       collapsed = copy;
                   })
                   // 分组既是拖拽源（拖到别的分组 = 变其子分组），也是投放目标
                   // （接受请求/分组落入）。自身投放被谓词拒绝；成环由 store
                   // 环检测兜底（toast 报错）。落盘统一推迟出指针事件路径。
                   // 鼠标按下即拖 + 拖影 preview，理由同请求行。
                   .With(huxerui::DragSource(
                             GroupDragPayload{g.id},
                             [name = g.name, bg = theme.colors.surface_container_high,
                              fg = theme.colors.on_surface] {
                                 return huxerui::Row {
                                     huxerui::Text("▾ " + (name.empty() ? "（未命名）" : name),
                                                   huxerui::TextRole::Body),
                                 }
                                     .With(huxerui::Padding(6.0F), huxerui::Background(bg),
                                           huxerui::Foreground(fg), huxerui::CornerRadius(6.0F));
                             }),
                         huxerui::DropTarget::Accepts<RequestDragPayload>(),
                         huxerui::DropTarget::Accepts<GroupDragPayload>(
                             [gid = g.id](const GroupDragPayload& p) { return p.groupId != gid; }))
                   .On<huxerui::DropEvents<RequestDragPayload>::Dropped>(
                       [tasks, toast, listVersion, gid = g.id](const RequestDragPayload& p,
                                                               const huxerui::DropEvent&) {
                           tasks.Launch([=]() -> huxerui::Task<void> {
                               co_await huxerui::Delay(std::chrono::duration<double>{0});
                               if (const std::string err =
                                       g_requests.moveToGroup(p.requestId, gid);
                                   !err.empty()) {
                                   toast.Show("移动失败: " + err);
                                   co_return;
                               }
                               listVersion = listVersion.Get() + 1;
                           });
                       })
                   .On<huxerui::DropEvents<GroupDragPayload>::Dropped>(
                       [tasks, toast, listVersion, gid = g.id](const GroupDragPayload& p,
                                                               const huxerui::DropEvent&) {
                           tasks.Launch([=]() -> huxerui::Task<void> {
                               co_await huxerui::Delay(std::chrono::duration<double>{0});
                               if (const std::string err = g_requests.moveGroup(p.groupId, gid);
                                   !err.empty()) {
                                   toast.Show("移动失败: " + err);
                                   co_return;
                               }
                               listVersion = listVersion.Get() + 1;
                           });
                       })
                   // 悬停显隐 ⋮：理由同请求行（Enter 记 -g.id，Leave 条件清空）。
                   .On<huxerui::ViewEvents::Hover>(
                       [hoveredRow, key = -g.id](const huxerui::HoverEvent& e) {
                           if (e.type == huxerui::HoverEventType::Enter)
                               hoveredRow = key;
                           else if (e.type == huxerui::HoverEventType::Leave &&
                                    hoveredRow.Get() == key)
                               hoveredRow = 0;
                       })
                   // 右键菜单：条目同 ⋮ 按钮，跟随点击位置弹出（理由同请求行）。
                   .On<huxerui::ViewEvents::ContextMenuRequested>(
                       [ctxMenu, groupEntries, g](huxerui::Point pos) {
                           ShowAppMenuAt(ctxMenu, pos, groupEntries(g));
                       })
                   // 稳定 Key：理由同请求行（悬停重组时保留节点与其扩展实例）。
                   // 取负与请求行 Key 区分。
                   .Key(-g.id);
    };

    // 递归展开：先子分组（未折叠才展开其子树），再本分组内的请求。
    // parentId=0 一轮即根层：顶层分组 + 未分组请求。
    std::function<void(std::int64_t, int)> emitLevel = [&](std::int64_t parentId, int depth) {
        for (const db::Group& g : groups) {
            if (g.parentId != parentId) continue;
            const bool isCollapsed = std::ranges::contains(collapsed.Get(), g.id);
            rows.push_back(groupRow(g, depth, isCollapsed));
            if (!isCollapsed) emitLevel(g.id, depth + 1);
        }
        for (const db::SavedRequest& r : saved) {
            if (r.groupId == parentId) rows.push_back(requestRow(r, depth));
        }
    };
    emitLevel(0, 0);

    if (saved.empty() && groups.empty()) {
        rows.push_back(huxerui::Text("集合为空：在右侧新建请求并保存。",
                                     huxerui::TextRole::Body)
                           .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }

    // 头部行：标题 + 圆形 "+"（自绘 ShowPopupMenu 卡片菜单，与全项目菜单观感
    // 统一；官方 menu.Show 已弃用）。不再显示项目名——顶级标签条已标识当前
    // 项目。菜单项回调在菜单层关闭后执行，不卸载被点按钮（按钮在本岛，菜单
    // 在层上），可直接写 State。
    // 独立浮动 "+"：保持 Circular + compact 档（28pt）+ accent 底，视觉不变。
    huxerui::View island = huxerui::Column {
                               huxerui::Row {
                                   huxerui::Text("请求", huxerui::TextRole::Title)
                                       .With(huxerui::Grow(1.0F)),
                                   AppIconButton("+", "新建请求",
                                                 [addPopup, dialog, tasks, toast, drafts,
                                                  activeTab, listVersion, newGroupName,
                                                  newGroupPath] {
                                       auto pushDraft = [drafts, activeTab](int kind) {
                                           std::vector<RequestDraft> copy = drafts.Get();
                                           copy.push_back(RequestDraft{.kind = kind});
                                           drafts = copy;
                                           activeTab = copy.size() - 1;
                                       };
                                       // 三组条目（后两组带分隔线）：新建请求类型（四条平铺；
                                       // kind 3 = gRPC 占位，不落库）/ 新建接口目录 / 导入接口。
                                       std::vector<PopupMenuItem> items;
                                       items.push_back(PopupMenuItem{
                                           .label = "HTTP 请求",
                                           .on_click = [pushDraft] { pushDraft(0); }});
                                       items.push_back(PopupMenuItem{
                                           .label = "WebSocket",
                                           .on_click = [pushDraft] { pushDraft(1); }});
                                       items.push_back(PopupMenuItem{
                                           .label = "TCP",
                                           .on_click = [pushDraft] { pushDraft(2); }});
                                       items.push_back(PopupMenuItem{
                                           .label = "gRPC 请求",
                                           .on_click = [pushDraft] { pushDraft(3); }});
                                       items.push_back(PopupMenuItem{
                                           .label = "新建接口目录…",
                                           .on_click = [dialog, tasks, toast, listVersion,
                                                        newGroupName, newGroupPath] {
                                               newGroupName = huxerui::TextEditingValue{};
                                               newGroupPath = huxerui::TextEditingValue{};
                                               dialog.Show(
                                                   [tasks, toast, listVersion, newGroupName,
                                                    newGroupPath](huxerui::DialogContext ctx)
                                                       -> huxerui::View {
                                                       return DialogCard(huxerui::Column {
                                                           huxerui::Text(
                                                               "新建接口目录",
                                                               huxerui::TextRole::Title),
                                                           huxerui::TextField(newGroupName.Get())
                                                               .Label("名称")
                                                               .Variant(huxerui::TextFieldVariant::Outlined)
                                                               .OnChanged([newGroupName](
                                                                      const huxerui::TextEditingValue&
                                                                          value) {
                                                                   newGroupName = value;
                                                               }),
                                                           // 路径可空 = 仅名称（Name 模式）；
                                                           // 斜杠分段 = Path 模式（URL 前缀）。
                                                           // 填路径时名称为空 → 路径同步进名称；
                                                           // 名称已非空 → 路径不再动名称。
                                                           huxerui::TextField(newGroupPath.Get())
                                                               .Label("路径")
                                                               .Placeholder("api/v1（斜杠分段；留空则仅作名称）")
                                                               .Variant(huxerui::TextFieldVariant::Outlined)
                                                               .OnChanged([newGroupName, newGroupPath](
                                                                      const huxerui::TextEditingValue&
                                                                          value) {
                                                                   newGroupPath = value;
                                                                   if (newGroupName.Get().text.empty())
                                                                       newGroupName = value;
                                                               }),
                                                           huxerui::Row {
                                                               huxerui::Button("取消")
                                                                   .OnClick([ctx] {
                                                                       ctx.Dismiss();
                                                                   }),
                                                               huxerui::Button("创建")
                                                                   .OnClick([ctx, tasks, toast,
                                                                             listVersion,
                                                                             newGroupName,
                                                                             newGroupPath] {
                                                                       const std::string name =
                                                                           newGroupName.Get().text;
                                                                       const std::string path =
                                                                           newGroupPath.Get().text;
                                                                       if (name.empty()) {
                                                                           toast.Show("名称不能为空");
                                                                           return;
                                                                       }
                                                                       ctx.Dismiss();
                                                                       // 建目录重组左岛：推迟出指针
                                                                       // 事件路径（约定 6）。
                                                                       tasks.Launch([=]() -> huxerui::Task<void> {
                                                                           co_await huxerui::Delay(
                                                                               std::chrono::duration<double>{0});
                                                                           if (const std::string err =
                                                                                   g_requests.createGroup(
                                                                                       name,
                                                                                       path.empty()
                                                                                           ? db::GroupMode::Name
                                                                                           : db::GroupMode::Path,
                                                                                       0, path);
                                                                               !err.empty()) {
                                                                               toast.Show("新建接口目录失败: " + err);
                                                                               co_return;
                                                                           }
                                                                           toast.Show("已新建接口目录");
                                                                           listVersion = listVersion.Get() + 1;
                                                                       });
                                                                   }),
                                                           }
                                                               .With(huxerui::MainAlign(
                                                                   huxerui::MainAxisAlignment::SpaceBetween)),
                                                       }
                                                           .With(huxerui::Spacing(12.0F),
                                                                 huxerui::Frame{.width = 320.0F},
                                                                 huxerui::CrossAlign(
                                                                     huxerui::CrossAxisAlignment::Stretch)));
                                                   },
                                                   huxerui::DialogOptions{});
                                           }});
                                       items.push_back(PopupMenuItem{
                                           .label = "导入接口…",
                                           .on_click = [dialog, listVersion] {
                                               dialog.Show(
                                                   [listVersion](huxerui::DialogContext ctx)
                                                       -> huxerui::View {
                                                       return ApiImportDialogContent(ctx,
                                                                                     listVersion);
                                                   },
                                                   huxerui::DialogOptions{});
                                           }});
                                       ShowPopupMenu(addPopup, std::move(items),
                                                     huxerui::PopupOptions{
                                                         .placement = {huxerui::AnchorSide::Below,
                                                                       huxerui::AnchorAlignment::Start}});
                                   },
                                   AppIconButtonShape::Circular, 28.0F, /*accent=*/true)
                                       .With(addPopup.Anchor()),
                               }
                                   .With(huxerui::CrossAlign(
                                       huxerui::CrossAxisAlignment::Center)),
                               // 根投放区：落到列表空白/非分组行上 = 移到根目录
                               // （请求移出分组，分组回到顶层）。
                               huxerui::ScrollView{
                                   huxerui::Column(std::move(rows))
                                       .With(huxerui::Spacing(theme.spacing.small))
                                       .With(huxerui::DropTarget::Accepts<RequestDragPayload>(),
                                             huxerui::DropTarget::Accepts<GroupDragPayload>())
                                       .On<huxerui::DropEvents<RequestDragPayload>::Dropped>(
                                           [tasks, toast, listVersion](
                                               const RequestDragPayload& p,
                                               const huxerui::DropEvent&) {
                                               tasks.Launch([=]() -> huxerui::Task<void> {
                                                   co_await huxerui::Delay(
                                                       std::chrono::duration<double>{0});
                                                   if (const std::string err =
                                                           g_requests.moveToGroup(p.requestId, 0);
                                                       !err.empty()) {
                                                       toast.Show("移动失败: " + err);
                                                       co_return;
                                                   }
                                                   listVersion = listVersion.Get() + 1;
                                               });
                                           })
                                       .On<huxerui::DropEvents<GroupDragPayload>::Dropped>(
                                           [tasks, toast, listVersion](
                                               const GroupDragPayload& p,
                                               const huxerui::DropEvent&) {
                                               tasks.Launch([=]() -> huxerui::Task<void> {
                                                   co_await huxerui::Delay(
                                                       std::chrono::duration<double>{0});
                                                   if (const std::string err =
                                                           g_requests.moveGroup(p.groupId, 0);
                                                       !err.empty()) {
                                                       toast.Show("移动失败: " + err);
                                                       co_return;
                                                   }
                                                   listVersion = listVersion.Get() + 1;
                                               });
                                           })}
                                   .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
                           }
                               .With(huxerui::Padding(theme.spacing.medium),
                                     huxerui::Spacing(theme.spacing.medium),
                                     huxerui::Background(theme.colors.surface_container_low),
                                     huxerui::CornerRadius(theme.shapes.large),
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    // 方向相关尺寸：竖排（Compact）限高撑宽；横排固定宽 260。
    return vertical ? std::move(island).With(huxerui::Frame{.max_height = 220.0F})
                    : std::move(island).With(huxerui::Frame{.width = 260.0F});
}

} // namespace apitab::ui
