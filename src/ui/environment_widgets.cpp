// environment_widgets.cpp — 环境配置弹窗（左岛环境列表 + 右侧表单）。
// 自 request_page.cpp 拆出（P1-C1，功能域 = 环境控件），纯搬移；
// KvTable 经 ui.h 复用 request_editor.cpp 的单一 owner 实现。
#include <huxerui/huxerui.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "ui.h"
#include "draft.h"

import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.utils;

namespace apitab::ui {

// 与 request_editor.cpp 的同名桥接同逻辑，签名含模块类型 api::KeyValue，
// 普通头无法声明（CLAUDE.md 模块约束）；P1-C3 归并唯一实现。此处按需并列。
inline api::KeyValue ToKeyValue(const KvRow& row) {
    return api::KeyValue{.key = row.key.text,
                         .value = row.value.text,
                         .enabled = row.enabled,
                         .type = row.type.text,
                         .remark = row.remark.text};
}

inline KvRow FromKeyValue(const api::KeyValue& kv) {
    return KvRow{.key = huxerui::TextEditingValue{kv.key},
                 .value = huxerui::TextEditingValue{kv.value},
                 .type = huxerui::TextEditingValue{kv.type},
                 .remark = huxerui::TextEditingValue{kv.remark},
                 .enabled = kv.enabled};
}

// ---- 环境配置弹窗 ----

// 弹窗右侧：选中环境的配置表单（名称 / 基础 URL / 环境变量 KV 表 + 保存/关闭）。
// 以 envId 为 Key 挂进弹窗：切换选中环境时整个表单作用域重建，UseState 初始值
// 随之重取（初始值在 UseState 之前算好，组合体内不写 State）。
[[huxerui::composable]] huxerui::View EnvEditForm(huxerui::DialogContext ctx,
                                                  std::int64_t envId,
                                                  huxerui::State<int> envVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto toast = huxerui::UseToast();
    const db::Environment* env = g_requests.findEnvironment(envId);
    if (env == nullptr) {
        // 防御：选中项刚被删除（正常路径在删除回调里已回落 selectedId）。
        return huxerui::Text("（环境已删除）", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant));
    }
    auto name = huxerui::UseState(huxerui::TextEditingValue{env->name});
    auto baseUrl = huxerui::UseState(huxerui::TextEditingValue{env->baseUrl});
    std::vector<KvRow> initialVars;
    for (const api::KeyValue& kv : env->variables) initialVars.push_back(FromKeyValue(kv));
    auto vars = huxerui::UseState<std::vector<KvRow>>(std::move(initialVars));

    return huxerui::Column {
        huxerui::ScrollView{huxerui::Column {
            huxerui::TextField(name.Get())
                .Label("名称")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([name](const huxerui::TextEditingValue& value) { name = value; }),
            huxerui::TextField(baseUrl.Get())
                .Label("基础 URL")
                .Placeholder("https://api.example.com")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([baseUrl](const huxerui::TextEditingValue& value) { baseUrl = value; }),
            huxerui::Text("环境变量（请求里用 {{变量名}} 引用）", huxerui::TextRole::Label),
            KvTable(vars.Get(), theme, "变量名", "变量值",
                    [vars](std::vector<KvRow> rows) { vars = std::move(rows); }),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
        huxerui::Row {
            // 保存不卸载本按钮（表单 Key 不变、State 保留）：同步写即可。
            huxerui::Button("保存").OnClick([ctx, envId, name, baseUrl, vars, envVersion, toast] {
                if (name.Get().text.empty()) {
                    toast.Show("环境名称不能为空");
                    return;
                }
                std::vector<api::KeyValue> kvs;
                kvs.reserve(vars.Get().size());
                for (const KvRow& row : vars.Get()) kvs.push_back(ToKeyValue(row));
                if (const std::string err = g_requests.updateEnvironment(
                        envId, name.Get().text, baseUrl.Get().text, kvs);
                    !err.empty()) {
                    toast.Show("保存失败: " + err);
                    return;
                }
                toast.Show("已保存");
                envVersion = envVersion.Get() + 1;
                ctx.Dismiss();
            }),
            huxerui::Button("关闭").OnClick([ctx] { ctx.Dismiss(); }),
        }
            // 两端对齐：保存在左、关闭在右，与其他弹窗一致。
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

// 环境配置弹窗（☰ 打开）：左侧环境列表（点击选中 / ＋ 新建 / ✎ 重命名 / ✕ 删除），
// 右侧选中环境的配置表单。envVersion 由 RequestPage 持有：环境增删改与保存后 bump，
// 弹窗与标签栏的环境下拉都按它重读 store。重命名输入框与删除确认框叠在本弹窗层之上
// （层内容捕获页面环境，UseDialog 照常可用）。
[[huxerui::composable]] huxerui::View EnvironmentDialog(huxerui::DialogContext ctx,
                                                        huxerui::State<int> envVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto toast = huxerui::UseToast();
    auto tasks = huxerui::UseTaskScope();
    auto dialog = huxerui::UseDialog();
    auto selectedId = huxerui::UseState<std::int64_t>(g_requests.currentEnvId());
    // 悬停行 id（0 = 无）：Hover 事件非独占，悬停 ✎/✕ 时整行底色照样亮；
    // Leave 仅当仍是本行才清，防跨行误清。
    auto hoveredEnv = huxerui::UseState<std::int64_t>(0);
    auto renameValue = huxerui::UseState(huxerui::TextEditingValue{});
    (void)envVersion.Get(); // 订阅环境版本：增删改/保存后重组本弹窗

    const std::vector<db::Environment>& envs = g_requests.environments();
    // 选中项失效（被删）时的只读回落；正常路径由删除回调直接重置 selectedId。
    std::int64_t effective = selectedId.Get();
    if (effective != 0 && !g_requests.findEnvironment(effective))
        effective = g_requests.currentEnvId();

    std::vector<huxerui::View> rows;
    for (const db::Environment& e : envs) {
        const std::int64_t id = e.id;
        const bool selected = id == effective;
        rows.push_back(
            huxerui::Row {
                // 选中区：名称占满行宽；点击挂在整行 Row 上（见下方 .OnClick），
                // ✎/✕ 是最深命中节点、点击不冒泡，各触发各的。
                huxerui::Text(e.name.empty() ? "（未命名）" : e.name, huxerui::TextRole::Body)
                    .With(huxerui::Grow(1.0F), huxerui::ClipChildren(),
                          huxerui::Foreground(selected ? theme.colors.on_surface
                                                       : theme.colors.on_surface_variant)),
                // ✎ 重命名：弹输入框小弹窗（renameValue 寄宿本弹窗作用域）。
                AppIconButton("✎", "重命名环境", [dialog, tasks, toast, renameValue, envVersion, id,
                              name = e.name] {
                        renameValue = huxerui::TextEditingValue{name};
                        dialog.Show(
                            [tasks, toast, renameValue, envVersion,
                             id](huxerui::DialogContext renameCtx) -> huxerui::View {
                                return DialogCard(huxerui::Column {
                                    huxerui::Text("重命名环境", huxerui::TextRole::Title),
                                    huxerui::TextField(renameValue.Get())
                                        .Label("环境名称")
                                        .Variant(huxerui::TextFieldVariant::Outlined)
                                        .OnChanged([renameValue](
                                                       const huxerui::TextEditingValue& value) {
                                            renameValue = value;
                                        }),
                                    huxerui::Row {
                                        huxerui::Button("取消").OnClick(
                                            [renameCtx] { renameCtx.Dismiss(); }),
                                        huxerui::Button("确定")
                                            .OnClick([renameCtx, tasks, toast, renameValue,
                                                      envVersion, id] {
                                                if (renameValue.Get().text.empty()) {
                                                    toast.Show("环境名称不能为空");
                                                    return;
                                                }
                                                renameCtx.Dismiss();
                                                // 重组弹窗内容：推迟出指针事件路径
                                                tasks.Launch([=]() -> huxerui::Task<void> {
                                                    co_await huxerui::Delay(
                                                        std::chrono::duration<double>{0});
                                                    if (const std::string err =
                                                            g_requests.renameEnvironment(
                                                                id, renameValue.Get().text);
                                                        !err.empty()) {
                                                        toast.Show("重命名失败: " + err);
                                                        co_return;
                                                    }
                                                    envVersion = envVersion.Get() + 1;
                                                });
                                            }),
                                    }
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
                    }, AppIconButtonShape::Bare),
                // ✕ 删除：危险确认框（共享 helper，确认按钮染红）；删除重组本弹窗 → 推迟。
                AppIconButton("✕", "删除环境", [dialog, tasks, selectedId, envVersion, id,
                                                      name = e.name] {
                        ShowDangerConfirm(dialog, "删除环境",
                                          "确定删除环境「" + name + "」吗？此操作不可恢复。",
                                          "删除",
                                          [tasks, selectedId, envVersion, id] {
                                              tasks.Launch([=]() -> huxerui::Task<void> {
                                                  co_await huxerui::Delay(
                                                      std::chrono::duration<double>{0});
                                                  (void)g_requests.deleteEnvironment(id);
                                                  if (selectedId.Get() == id)
                                                      selectedId = g_requests.currentEnvId();
                                                  envVersion = envVersion.Get() + 1;
                                              });
                                          });
                    }, AppIconButtonShape::Bare),
            }
                .With(huxerui::Spacing(0.0F),
                      huxerui::Padding(huxerui::EdgeInsets::Symmetric(6.0F, 4.0F)),
                      // 与设置分类等选择列表统一：常态透明、悬停 raised、选中 active；
                      // 选中优先，悬停选中行不会退回另一种底色。
                      huxerui::Background(selected ? islands.active
                                          : hoveredEnv.Get() == id ? islands.raised
                                                                   : huxerui::Color::Transparent()),
                      huxerui::CornerRadius(islands.nested_radius),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      // 行自身压掉默认 Indication：悬停反馈由手工底色承担，避免叠加。
                      huxerui::Indication{})
                // 点击整行 = 选中（只换右侧表单 Key，不卸载本行）：同步写即可。
                .OnClick([selectedId, id] { selectedId = id; })
                .On<huxerui::ViewEvents::Hover>([hoveredEnv, id](const huxerui::HoverEvent& e) {
                    if (e.type == huxerui::HoverEventType::Enter)
                        hoveredEnv = id;
                    else if (e.type == huxerui::HoverEventType::Leave && hoveredEnv.Get() == id)
                        hoveredEnv = 0;
                })
                .Key(id));
    }
    if (rows.empty()) {
        rows.push_back(huxerui::Text("暂无环境，点击上方 + 新建。", huxerui::TextRole::Body)
                           .With(huxerui::Foreground(theme.colors.on_surface_variant)));
    }

    // 右侧表单：Key = 选中环境 id，切换选中即重建表单作用域（初始值重取）。
    // effective==0 = 环境下拉选了"无"：无表单可编辑，只给提示。
    huxerui::View form = huxerui::Column{};
    if (envs.empty()) {
        form = huxerui::Text("暂无环境", huxerui::TextRole::Body)
                   .With(huxerui::Foreground(theme.colors.on_surface_variant));
    } else if (effective == 0) {
        form = huxerui::Text("未选择环境（当前为“无”）。", huxerui::TextRole::Body)
                   .With(huxerui::Foreground(theme.colors.on_surface_variant));
    } else {
        form = EnvEditForm(ctx, effective, envVersion).Key(effective);
    }

    return DialogCard(huxerui::Column {
        huxerui::Text("环境配置", huxerui::TextRole::Title),
        huxerui::Row {
            huxerui::Column {
                huxerui::Row {
                    huxerui::Text("环境", huxerui::TextRole::Label)
                        .With(huxerui::Grow(1.0F)),
                    // ＋ 新建：store 建默认名的环境并选中，随后可在右侧表单改名。
                    // 独立浮动 "+"：保持 Circular + compact 档（28pt），视觉不变。
                    AppIconButton("+", "新建环境", [tasks, toast, selectedId, envVersion] {
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double>{0});
                            if (const std::string err =
                                    g_requests.createEnvironment("新环境", "");
                                !err.empty()) {
                                toast.Show("新建环境失败: " + err);
                                co_return;
                            }
                            selectedId = g_requests.currentEnvId();
                            envVersion = envVersion.Get() + 1;
                        });
                    }, AppIconButtonShape::Circular, 28.0F, /*accent=*/false),
                }
                    .With(huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                huxerui::ScrollView{huxerui::Column(std::move(rows))
                                        .With(huxerui::Spacing(theme.spacing.small))}
                    .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::Frame{.width = 200.0F},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
            std::move(form),
        }
            .With(huxerui::Spacing(theme.spacing.medium), huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)),
    }
                          .With(huxerui::Spacing(12.0F),
                                huxerui::Frame{.width = 680.0F, .height = 460.0F},
                                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

} // namespace apitab::ui
