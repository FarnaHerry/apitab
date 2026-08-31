// settings_page.cpp — 全局设置：主题模式（官方 Select 下拉）+ 关闭行为。
// 选择都持久化到 settings.ini（会话偏好）。
#include <huxerui/huxerui.h>

#include <memory>
#include <string>
#include <vector>

#include "ui.h"

import apitab.config;
import apitab.preferences;

namespace apitab::ui {

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
        PageHeader("全局设置", "外观与应用行为（保存在 settings.ini）"),
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
