// settings_page.cpp — 全局设置：主题模式（官方 Select 下拉）+ 关闭行为 +
// 默认请求超时 + 全局公共请求头。选择都持久化到 settings.ini（会话偏好）；
// 超时/公共头写入 session.request_timeout_sec / session.global_headers 键，
// 发送侧在 store 的 finalizeSpec 统一读取生效（见 src/store/requests.cppm）。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "draft.h"

import apitab.config;
import apitab.preferences;
import nlohmann.json;

namespace apitab::ui {

namespace {

// 全局公共头数组 → 单行紧凑 JSON（{"k","v","on","t","r"}，与 db 侧同格式；
// t/r 恒写空串——store 的 parseSettingKv 按这五个键读）。ini 是行式存储，
// dump() 无缩进且转义串内换行，结果必为单行。
std::string SerializeGlobalHeaders(const std::vector<KvRow>& rows) {
    nlohmann::json arr = nlohmann::json::array();
    for (const KvRow& row : rows) {
        nlohmann::json::object_t obj;
        obj["k"] = row.key.text;
        obj["v"] = row.value.text;
        obj["on"] = row.enabled;
        obj["t"] = "";
        obj["r"] = "";
        arr.push_back(std::move(obj));
    }
    return arr.dump();
}

// settings.ini 的 global_headers 文本 → KvRow 数组（非法/非数组/字段类型
// 不符一律按空表处理，与 store 侧读取兜底一致）。
std::vector<KvRow> ParseGlobalHeaders(const std::string& text) {
    std::vector<KvRow> out;
    try {
        const nlohmann::json arr = nlohmann::json::parse(text, nullptr, false);
        if (!arr.is_array()) return {};
        for (const auto& item : arr) {
            if (!item.is_object()) continue;
            KvRow row;
            row.enabled = item.value("on", true);
            row.key.text = item.value("k", std::string {});
            row.value.text = item.value("v", std::string {});
            row.type.text = item.value("t", std::string {});
            row.remark.text = item.value("r", std::string {});
            out.push_back(std::move(row));
        }
    } catch (...) {
        return {};
    }
    return out;
}

// 全局公共头表：MockHeaderTable 同语义的两列 KV 表（启用 Checkbox / 键 /
// 值 / ✕）——末尾恒渲染一个虚拟空行，对它写入键或值即物化为真实行（仅
// 聚焦/移动光标触发的空 OnChanged 不追加）；✕ 只给真实行，删除会卸载本
// 按钮所在行，推迟出指针事件路径（CLAUDE.md 约定 6）。
// KvRow 的 type/remark 不使用（序列化时写空串）。
[[huxerui::composable]] huxerui::View GlobalHeadersTable(
    std::vector<KvRow> rows, const huxerui::ThemeSpec& theme,
    std::function<void(std::vector<KvRow>)> onChanged) {
    auto tasks = huxerui::UseTaskScope();
    std::vector<huxerui::View> children{
        huxerui::Row {
            huxerui::Text("", huxerui::TextRole::Label)
                .With(huxerui::Frame{.width = 24.0F}),
            huxerui::Text("头名称", huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
            huxerui::Text("头值", huxerui::TextRole::Label)
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Foreground(theme.colors.on_surface_variant)),
    };
    for (std::size_t i = 0; i <= rows.size(); ++i) {
        const bool phantom = i == rows.size();
        const KvRow row = phantom ? KvRow {} : rows[i];
        // 行写入：i 越界（虚拟行）时物化新行，否则改写原行。
        auto applyRow = [rows, onChanged](std::size_t i, KvRow updated) {
            std::vector<KvRow> copy = rows;
            if (i < copy.size()) {
                copy[i] = std::move(updated);
            } else {
                if (updated.key.text.empty() && updated.value.text.empty()) return;
                copy.push_back(std::move(updated));
            }
            onChanged(std::move(copy));
        };
        children.push_back(
            huxerui::Row {
                huxerui::Checkbox(row.enabled).OnChanged([row, i, applyRow](bool checked) {
                    KvRow updated = row;
                    updated.enabled = checked;
                    applyRow(i, std::move(updated));
                }),
                huxerui::TextField(row.key)
                    .Label("键")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.key = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                huxerui::TextField(row.value)
                    .Label("值")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                        KvRow updated = row;
                        updated.value = value;
                        applyRow(i, std::move(updated));
                    })
                    .With(huxerui::Grow(1.0F)),
                phantom
                    ? huxerui::View {huxerui::Text("", huxerui::TextRole::Label)
                                         .With(huxerui::Padding(4.0F))}
                    : huxerui::View {huxerui::Button("✕").OnClick([tasks, rows, i, onChanged] {
                        // 删除会卸载本按钮所在行：推迟出指针事件路径
                        tasks.Launch([=]() -> huxerui::Task<void> {
                            co_await huxerui::Delay(std::chrono::duration<double> {0});
                            std::vector<KvRow> copy = rows;
                            if (i < copy.size()) copy.erase(copy.begin() + static_cast<long>(i));
                            onChanged(std::move(copy));
                        });
                    })},
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    }
    return huxerui::Column(std::move(children))
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace

[[huxerui::composable]] huxerui::View GlobalSettingsPage(huxerui::State<int> themeMode,
                                                         huxerui::State<int> closeBehavior) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    // 场景过渡：主题切换（整树换色）用圆形揭示动画包一层；reduced motion
    // 由 handle 自行降级，mutation 在无视觉效果时也必须正确落盘。
    auto transition = huxerui::UseSceneTransition();
    // 动画冻结标记：场景过渡没有"运行中"查询/完成回调，重复触发会打断上一个
    // 动画（很难看）。app 侧用定时器（略长于默认 Tween 0.36s）模拟冷却期，
    // 期间按钮再按无反应。
    struct FlagCell {
        bool animating = false;
    };
    auto animating = huxerui::UseState(std::make_shared<FlagCell>());

    // 默认请求超时：数字文本承载（受控 TextEditingValue 全量保留），初始读
    // session.request_timeout_sec，空/非法在发送侧按 30 秒兜底。OnChanged 即
    // 写回 ini（写文件很轻，不必防抖）。
    auto timeoutSec = huxerui::UseState(
        huxerui::TextEditingValue {sessionPreference("request_timeout_sec")});
    // 全局公共请求头：初始解析 session.global_headers（单行 JSON，见上）。
    // 任何增删改都全量序列化后落盘，store 的 finalizeSpec 每次发送现读。
    auto globalHeaders =
        huxerui::UseState(ParseGlobalHeaders(sessionPreference("global_headers")));

    // 切换主题模式（0=跟随系统 1=深色 2=浅色）。检测当前有效深浅：
    // 目标与现状一致（含"跟随系统"已等于系统偏好）时直接落盘、不放动画。
    // 动画原点 = 本次点击的指针位置（RunFromCurrentInteraction 取当前事件
    // 派发的精确坐标；键盘激活时回落到锚点 View 中心）。深→浅的"收束"方向
    // 上游刻意不提供（场景过渡不支持反向播放），两个方向都用展开。
    auto applyTheme = [themeMode, transition, tasks, animating](int mode) {
        if (animating.Get()->animating) return; // 动画进行中：冻结，再按无反应
        const bool currentDark =
            themeMode.Get() == 1 || (themeMode.Get() == 0 && cfg::systemPrefersDark());
        const bool targetDark = mode == 1 || (mode == 0 && cfg::systemPrefersDark());
        auto mutation = [themeMode, mode] {
            themeMode = mode;
            saveSessionPreference("theme_mode", std::to_string(themeMode.Get()));
        };
        if (currentDark == targetDark) {
            mutation();
            return;
        }
        animating.Get()->animating = true;
        tasks.Launch([animating]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0.5});
            animating.Get()->animating = false;
        });
        // 必须在同步事件回调内调用（Select OnChanged 满足）；mutation 由过渡
        // 服务在换帧点调用（已在指针事件路径之外），无需再手动推迟。
        transition.RunFromCurrentInteraction(huxerui::CircularRevealSceneTransition{},
                                             std::move(mutation));
    };

    // 岛屿分区模型：岛本身占满整个页面区块（Grow + Stretch），内容在岛内部滚动。
    return huxerui::Column {
        PageHeader("全局设置",
                   "外观与应用行为（保存在 settings.ini）；含请求默认超时与全局公共头"),
        huxerui::ScrollView{huxerui::Column {
            huxerui::Text("主题模式", huxerui::TextRole::Label),
            // 官方下拉选择（下标即 themeMode：0=跟随系统 1=深色 2=浅色）；
            // 动画原点 = 点中菜单项的指针位置。
            huxerui::Row {
                huxerui::Select(std::vector<std::string>{"跟随系统", "深色模式", "浅色模式"},
                                static_cast<std::size_t>(themeMode.Get()),
                                [](const std::string& option) {
                                    return huxerui::Text(option).Key(option);
                                })
                    .OnChanged([applyTheme](std::size_t index) {
                        applyTheme(static_cast<int>(index));
                    }),
            },
            huxerui::Text("跟随系统会读取当前桌面的深浅色偏好。", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            huxerui::Text("关闭行为", huxerui::TextRole::Label),
            huxerui::Row {
                huxerui::Select(std::vector<std::string>{"每次询问", "直接关闭", "最小化到托盘"},
                                static_cast<std::size_t>(closeBehavior.Get()),
                                [](const std::string& option) {
                                    return huxerui::Text(option).Key(option);
                                })
                    .OnChanged([closeBehavior](std::size_t index) {
                        closeBehavior = static_cast<int>(index);
                        saveSessionPreference("close_behavior", std::to_string(closeBehavior.Get()));
                    }),
            },
            huxerui::Text("点标题栏 ✕ 时生效；“每次询问”在关闭时弹窗确认。", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            huxerui::Text("数据目录：~/.local/share/apitab", huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            huxerui::Text("默认请求超时（秒）", huxerui::TextRole::Label),
            huxerui::TextField(timeoutSec.Get())
                .Label("秒")
                .Placeholder("30")
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([timeoutSec](const huxerui::TextEditingValue& value) {
                    timeoutSec = value;
                    saveSessionPreference("request_timeout_sec", value.text);
                })
                .With(huxerui::Frame{.max_width = 240.0F}),
            huxerui::Text("作用于单次 HTTP 请求（curl 引擎），k6 压测不受影响；留空或非法按 "
                          "30 秒，下次发送即生效。",
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            huxerui::Text("全局公共请求头", huxerui::TextRole::Label),
            GlobalHeadersTable(globalHeaders.Get(), theme,
                               [globalHeaders](std::vector<KvRow> rows) {
                                   const std::string serialized = SerializeGlobalHeaders(rows);
                                   saveSessionPreference("global_headers", serialized);
                                   globalHeaders = std::move(rows);
                               }),
            huxerui::Text("发送给本请求集合的每个请求自动带上；请求里显式写了同名头"
                          "（大小写不敏感）则不重复注入。",
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            // 底部栏快捷入口提示（按钮本体在别处并行实现，此处只给指引文案）。
            huxerui::Text("请求代理与全局 Cookie 的快捷配置在窗口底部栏「请求代理」「全局 "
                          "Cookie」按钮。",
                          huxerui::TextRole::Body)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }
                                .With(huxerui::Spacing(theme.spacing.medium))}
            .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)),
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              // 过渡锚点：圆形揭示以本页面区域为画布（键盘激活时的原点兜底）。
              transition.Anchor());
}

} // namespace apitab::ui
