// apitab Windows 安装器界面 —— 逻辑对齐 Clash-Flux 安装器（品牌侧栏 +
// 全状态分页 + 深浅跟随系统 + 多语言），样式用主应用的极简主题：
// 大面积中性灰阶，品牌青只留给焦点环、eyebrow 与品牌侧栏渐变。
//
// 安装器是独立小 exe，不链接 apitab 模块：Minimal 深浅两套 ThemeSpec 与
// 主应用 src/ui/app.cpp 的 MinimalDarkThemeSpec/MinimalLightThemeSpec 同语义
// 本地实现（主应用改色时需同步）；主按钮的黑白极简样式复刻 MinimalThemeStyles
// 的 ButtonStyle 覆盖。字符串与 logo 资源由 huxerui_add_windows_installer 从
// package/resources 生成（RESOURCE_NAMESPACE apitab_installer）。
#include <algorithm>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <windows.h>

#include <apitab_installer_resources.h>
#include <huxerui/huxerui.h>
#include <huxerui/windows/installer.h>

using namespace huxerui;
using namespace huxerui::windows;
namespace installer_strings = apitab_installer::strings;

namespace {

// src/ui/ui.h 的 font_size 常量值（安装器不链接主模块，落成字面值）。
constexpr float kBodyFontSize = 14.0F;
constexpr float kChipFontSize = 12.0F;
constexpr float kTitleFontSize = 20.0F;

// ---- 极简主题（与主应用 MinimalDarkThemeSpec/MinimalLightThemeSpec 同步）----

ThemeSpec MinimalDarkThemeSpec() {
    ThemeSpec spec = MaterialDarkThemeSpec();
    spec.typography = TypographyScheme{.body_large = 16.0F,
                                       .body_medium = kBodyFontSize,
                                       .body_small = kChipFontSize,
                                       .label_large = kBodyFontSize,
                                       .title_large = kTitleFontSize,
                                       .headline_small = 26.0F};
    spec.shapes = ShapeScheme{.extra_small = 4.0F,
                              .small = 6.0F,
                              .medium = 8.0F,
                              .large = 12.0F,
                              .extra_large = 18.0F,
                              .full = 10000.0F};
    spec.spacing = SpacingScheme{.extra_small = 4.0F,
                                 .small = 8.0F,
                                 .medium = 12.0F,
                                 .large = 16.0F,
                                 .extra_large = 24.0F};
    spec.motion.fast = 0.16;
    spec.motion.normal = 0.20;
    spec.motion.slow = 0.30;
    // 深色：黑色画布、逐级提亮的中性表面；品牌青保持 #43D3DC。
    spec.colors.primary = Color::Rgb(67, 211, 220);
    spec.colors.on_primary = Color::Rgb(6, 37, 42);
    spec.colors.primary_container = Color::Rgb(43, 43, 43);
    spec.colors.on_primary_container = Color::Rgb(242, 242, 242);
    spec.colors.secondary = Color::Rgb(170, 170, 170);
    spec.colors.on_secondary = Color::Rgb(18, 18, 18);
    spec.colors.secondary_container = Color::Rgb(58, 58, 58);
    spec.colors.on_secondary_container = Color::Rgb(232, 232, 232);
    spec.colors.tertiary_container = Color::Rgb(48, 48, 48);
    spec.colors.on_tertiary_container = Color::Rgb(232, 232, 232);
    spec.colors.background = Color::Rgb(17, 17, 17);
    spec.colors.surface = Color::Rgb(24, 24, 24);
    spec.colors.surface_container_low = Color::Rgb(29, 29, 29);
    spec.colors.surface_container = Color::Rgb(36, 36, 36);
    spec.colors.surface_container_high = Color::Rgb(47, 47, 47);
    spec.colors.surface_container_highest = Color::Rgb(58, 58, 58);
    spec.colors.on_surface = Color::Rgb(242, 242, 242);
    spec.colors.on_surface_variant = Color::Rgb(170, 170, 170);
    spec.colors.outline = Color::Rgb(78, 78, 78);
    spec.colors.inverse_surface = Color::Rgb(242, 242, 242);
    spec.colors.inverse_on_surface = Color::Rgb(17, 17, 17);
    spec.colors.scrim = Color::Rgb(0, 0, 0, 0.55F);
    spec.colors.error = Color::Rgb(255, 142, 134);
    spec.interactions.focus_ring = FocusRing{spec.colors.primary, 2.0F, 2.0F};
    return spec;
}

ThemeSpec MinimalLightThemeSpec() {
    ThemeSpec spec = MaterialLightThemeSpec();
    spec.typography = TypographyScheme{.body_large = 16.0F,
                                       .body_medium = kBodyFontSize,
                                       .body_small = kChipFontSize,
                                       .label_large = kBodyFontSize,
                                       .title_large = kTitleFontSize,
                                       .headline_small = 26.0F};
    spec.shapes = ShapeScheme{.extra_small = 4.0F,
                              .small = 6.0F,
                              .medium = 8.0F,
                              .large = 12.0F,
                              .extra_large = 18.0F,
                              .full = 10000.0F};
    spec.spacing = SpacingScheme{.extra_small = 4.0F,
                                 .small = 8.0F,
                                 .medium = 12.0F,
                                 .large = 16.0F,
                                 .extra_large = 24.0F};
    spec.motion.fast = 0.16;
    spec.motion.normal = 0.20;
    spec.motion.slow = 0.30;
    // 浅色：白色内容岛放在浅灰画布上；品牌青保持 #28B8C7。
    spec.colors.primary = Color::Rgb(40, 184, 199);
    spec.colors.on_primary = Color::Rgb(6, 51, 58);
    spec.colors.primary_container = Color::Rgb(236, 236, 236);
    spec.colors.on_primary_container = Color::Rgb(28, 28, 28);
    spec.colors.secondary = Color::Rgb(96, 96, 96);
    spec.colors.on_secondary = Color::White();
    spec.colors.secondary_container = Color::Rgb(232, 232, 232);
    spec.colors.on_secondary_container = Color::Rgb(42, 42, 42);
    spec.colors.tertiary_container = Color::Rgb(240, 240, 240);
    spec.colors.on_tertiary_container = Color::Rgb(62, 62, 62);
    spec.colors.background = Color::Rgb(245, 245, 245);
    spec.colors.surface = Color::White();
    spec.colors.surface_container_low = Color::Rgb(250, 250, 250);
    spec.colors.surface_container = Color::Rgb(244, 244, 244);
    spec.colors.surface_container_high = Color::Rgb(236, 236, 236);
    spec.colors.surface_container_highest = Color::Rgb(227, 227, 227);
    spec.colors.on_surface = Color::Rgb(28, 28, 28);
    spec.colors.on_surface_variant = Color::Rgb(96, 96, 96);
    spec.colors.outline = Color::Rgb(210, 210, 210);
    spec.colors.inverse_surface = Color::Rgb(28, 28, 28);
    spec.colors.inverse_on_surface = Color::White();
    spec.colors.scrim = Color::Rgb(6, 32, 38, 0.42F);
    spec.colors.error = Color::Rgb(190, 65, 78);
    spec.interactions.focus_ring = FocusRing{spec.colors.primary, 2.0F, 2.0F};
    return spec;
}

// 安装器独立读取系统深浅色，不读 apitab 自身的主题偏好。
bool SystemPrefersDark() {
    HKEY raw_key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &raw_key) != ERROR_SUCCESS) {
        return false;
    }
    const std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(raw_key,
                                                                                   &RegCloseKey);

    DWORD apps_use_light_theme = 1;
    DWORD size = sizeof(apps_use_light_theme);
    const LONG result = RegQueryValueExA(key.get(), "AppsUseLightTheme", nullptr, nullptr,
                                         reinterpret_cast<LPBYTE>(&apps_use_light_theme), &size);
    return result == ERROR_SUCCESS && apps_use_light_theme == 0;
}

ThemeSpec InstallerThemeSpec() {
    return SystemPrefersDark() ? MinimalDarkThemeSpec() : MinimalLightThemeSpec();
}

// 主按钮的 apitab 极简样式：on_surface 黑/白底 + inverse_on_surface 字，
// medium 圆角（复刻主应用 MinimalThemeStyles 的 ButtonStyle 覆盖；安装器
// 只用到按钮，TextField/Checkbox/ProgressBar 走主题默认派生）。
[[huxerui::composable]]
View InstallerThemeStyles(View content) {
    const ThemeSpec& spec = UseTheme();
    ThemeDefinition definition;
    const auto withAlpha = [](Color c, float a) {
        c.alpha = a;
        return c;
    };

    ButtonStyle buttons = UseEnvironment<ButtonStyle>();
    buttons.background = spec.colors.on_surface;
    buttons.label_style = TextStyle{Font::System(kBodyFontSize), spec.colors.inverse_on_surface};
    buttons.disabled_background = withAlpha(spec.colors.on_surface, 0.10F);
    buttons.disabled_label = withAlpha(spec.colors.on_surface, 0.42F);
    buttons.padding = EdgeInsets::Symmetric(16.0F, 9.0F);
    buttons.minimum_height = 36.0F;
    buttons.corner_radii = CornerRadii{spec.shapes.medium};
    buttons.indication = Indication{
        .hover = IndicationLayer{.fill = withAlpha(spec.colors.inverse_on_surface, 0.10F)},
        .press = IndicationLayer{.fill = withAlpha(spec.colors.inverse_on_surface, 0.18F)},
    };
    definition.Set(buttons);
    return Theme{std::move(definition), std::move(content)};
}

std::string InstallPathText(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return {value.begin(), value.end()};
}

std::optional<std::filesystem::path> ParseInstallPath(std::string_view value) {
    if (value.empty()) return std::nullopt;
    try {
        std::filesystem::path path(std::u8string(value.begin(), value.end()));
        return path.is_absolute() ? std::optional<std::filesystem::path>{path.lexically_normal()}
                                  : std::nullopt;
    } catch (const std::filesystem::filesystem_error&) {
        return std::nullopt;
    }
}

// 品牌侧栏：品牌青对角渐变上放白圆角 logo + apitab 名，底部 WINDOWS 角标。
View InstallerMark() {
    return Image(apitab_installer::images::apitab_logo)
        .Fit(ImageFit::Contain)
        .With(Frame{.width = 64.0F, .height = 64.0F}, Padding(5.0F), Background(Color::White()),
              CornerRadius(18.0F), ClipChildren());
}

View BrandPanel(const ThemeSpec& theme) {
    Color deep_primary = theme.colors.primary;
    deep_primary.red *= 0.52F;
    deep_primary.green *= 0.52F;
    deep_primary.blue *= 0.52F;
    Color secondary_text = theme.colors.on_primary;
    secondary_text.alpha = 0.72F;

    return Column{InstallerMark(),
                  Column{Text(installer_strings::application_setup)
                             .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold),
                                              secondary_text}),
                         Text("apitab")
                             .Style(TextStyle{Font::System(28.0F).WithWeight(FontWeight::SemiBold),
                                              theme.colors.on_primary}),
                         Text(installer_strings::guided_setup)
                             .Style(TextStyle{Font::System(theme.typography.body_medium),
                                              secondary_text})}
                      .With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Start)),
                  Spacer().With(Grow()),
                  Text(installer_strings::platform_windows)
                      .Style(TextStyle{Font::System(11.0F).WithWeight(FontWeight::SemiBold),
                                       secondary_text})}
        .With(Frame{.width = 236.0F},
              Padding(32.0F),
              Spacing(30.0F),
              Background(LinearGradient{.start = {0.0F, 0.0F},
                                        .end = {1.0F, 1.0F},
                                        .stops = {{0.0F, theme.colors.primary},
                                                  {1.0F, deep_primary}}}),
              CrossAlign(CrossAxisAlignment::Start));
}

// 完成页对勾：圆底 + 圆头折线，主色 12% 透明圆盘。
View CompletionMark(const ThemeSpec& theme) {
    Color background = theme.colors.primary;
    background.alpha = 0.12F;
    return Canvas([background, foreground = theme.colors.primary](PaintContext& paint, Size) {
        paint.DrawCircle({26.0F, 26.0F}, 26.0F, background);
        const StrokeStyle stroke{.width = 3.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round};
        paint.DrawLine({15.0F, 27.0F}, {23.0F, 35.0F}, foreground, stroke);
        paint.DrawLine({23.0F, 35.0F}, {39.0F, 18.0F}, foreground, stroke);
    }).With(Frame{.width = 52.0F, .height = 52.0F});
}

// 次级按钮：中性灰底 + 常规字重，禁用态降透明（主按钮保持黑白极简）。
View SecondaryAction(View action, const ThemeSpec& theme) {
    Color disabled_background = theme.colors.on_surface;
    disabled_background.alpha = 0.08F;
    Color disabled_label = theme.colors.on_surface;
    disabled_label.alpha = 0.38F;
    ThemeDefinition definition;
    definition.Set(ButtonStyle{
        .background = theme.colors.surface_container_highest,
        .label_style = TextStyle{Font::System(theme.typography.label_large).WithWeight(FontWeight::Medium),
                                 theme.colors.on_surface},
        .disabled_background = disabled_background,
        .disabled_label = disabled_label,
        .padding = EdgeInsets::Symmetric(20.0F, 8.0F),
        .minimum_width = 72.0F,
        .minimum_height = 40.0F,
        .corner_radii = CornerRadii{theme.shapes.full},
        .indication = theme.interactions.indication,
    });
    return Theme{std::move(definition), std::move(action)};
}

// 引擎弹窗按钮组：推荐项走主按钮，其余次级，标签按枚举完整映射。
std::vector<View> PromptActions(const InstallerHandle& installer, const InstallerPrompt& prompt,
                                const ThemeSpec& theme) {
    std::vector<View> actions;
    for (const InstallerPromptChoice choice : prompt.choices) {
        StringVariant label = installer_strings::continue_action;
        switch (choice) {
        case InstallerPromptChoice::Ok: label = installer_strings::ok; break;
        case InstallerPromptChoice::Cancel: label = installer_strings::cancel; break;
        case InstallerPromptChoice::Abort: label = installer_strings::abort; break;
        case InstallerPromptChoice::Retry: label = installer_strings::retry; break;
        case InstallerPromptChoice::TryAgain: label = installer_strings::try_again; break;
        case InstallerPromptChoice::Ignore: label = installer_strings::ignore; break;
        case InstallerPromptChoice::Yes: label = installer_strings::yes; break;
        case InstallerPromptChoice::No: label = installer_strings::no; break;
        case InstallerPromptChoice::Continue: label = installer_strings::continue_action; break;
        }
        View action = Button(std::move(label)).OnClick([installer, id = prompt.id, choice] {
            installer.Respond(id, choice);
        });
        if (!prompt.recommended || choice != *prompt.recommended) {
            action = SecondaryAction(std::move(action), theme);
        }
        actions.push_back(std::move(action));
    }
    return actions;
}

[[huxerui::composable]]
View InstallerContent() {
    const InstallerHandle installer = UseInstaller();
    const WindowHandle window = UseWindow();
    const TaskScope tasks = UseTaskScope();
    const ThemeSpec& theme = UseTheme();
    const InstallerStatus status = installer.Status();
    auto destination = UseState<std::optional<TextEditingValue>>(std::nullopt);
    auto desktop_shortcut = UseState<std::optional<bool>>(std::nullopt);
    StringVariant eyebrow;
    StringVariant heading;
    std::vector<View> details;
    std::vector<View> actions;
    MainAxisAlignment panel_alignment = MainAxisAlignment::Start;

    if (status.prompt) {
        eyebrow = installer_strings::action_required;
        heading = installer_strings::setup_needs_attention;
        StringVariant prompt_message = status.prompt->message;
        if (status.prompt->kind == InstallerPromptKind::FilesInUse && status.prompt->message.empty()) {
            prompt_message = installer_strings::files_in_use;
        }
        details.push_back(
            Text(std::move(prompt_message)).With(Foreground(theme.colors.on_surface_variant)));
        actions = PromptActions(installer, *status.prompt, theme);
    } else if (status.phase == InstallerPhase::Detecting) {
        eyebrow = installer_strings::welcome;
        heading = installer_strings::preparing_setup;
        panel_alignment = MainAxisAlignment::Center;
        details.push_back(Text(installer_strings::checking_installed_version)
                              .With(Foreground(theme.colors.on_surface_variant)));
        details.push_back(ProgressBar(status.progress));
    } else if (status.phase == InstallerPhase::Ready &&
               status.product == InstallerProductState::NewerVersion) {
        // 这台机器上有更新的版本，本安装包是降级——只说明情况，不放行动作。
        eyebrow = installer_strings::version_check;
        heading = installer_strings::newer_version_installed;
        panel_alignment = MainAxisAlignment::Center;
        details.push_back(Text(installer_strings::remove_newer_version)
                              .With(Foreground(theme.colors.on_surface_variant)));
        actions.push_back(
            Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F}));
    } else if (status.phase == InstallerPhase::Ready &&
               status.product == InstallerProductState::Present) {
        // 旧版本升级页：主操作按已注册位置重铺新版本（Install 不触碰用户
        // 数据），修复与卸载作为次级选项。
        eyebrow = installer_strings::upgrade;
        heading = installer_strings::existing_installation_detected;
        details.push_back(Text(installer_strings::upgrade_keep_data_message)
                              .With(Foreground(theme.colors.on_surface_variant)));
        actions.push_back(SecondaryAction(
            Button(installer_strings::uninstall).OnClick([installer] { installer.Uninstall(); }), theme));
        actions.push_back(SecondaryAction(
            Button(installer_strings::repair).OnClick([installer] { installer.Repair(); }), theme));
        actions.push_back(Button(installer_strings::upgrade_keep_data)
                              .OnClick([installer] { installer.Install(); })
                              .With(Frame{.min_width = 152.0F}));
    } else if (status.phase == InstallerPhase::Ready &&
               status.product == InstallerProductState::SameVersion) {
        // 同版本维护页：重复打开同一版安装包不再当作升级，主操作按已注册
        // 位置重装并保留数据；修复与卸载保留为次级选项。
        eyebrow = installer_strings::maintenance;
        heading = installer_strings::same_version_installed;
        details.push_back(Text(installer_strings::same_version_message)
                              .With(Foreground(theme.colors.on_surface_variant)));
        actions.push_back(SecondaryAction(
            Button(installer_strings::uninstall).OnClick([installer] { installer.Uninstall(); }), theme));
        actions.push_back(SecondaryAction(
            Button(installer_strings::repair).OnClick([installer] { installer.Repair(); }), theme));
        actions.push_back(Button(installer_strings::reinstall_keep_data)
                              .OnClick([installer] { installer.Install(); })
                              .With(Frame{.min_width = 152.0F}));
    } else if (status.phase == InstallerPhase::Ready &&
               status.product == InstallerProductState::Absent) {
        const TextEditingValue destination_value =
            destination.Get().value_or(TextEditingValue::FromText(InstallPathText(status.default_destination)));
        const std::optional<std::filesystem::path> destination_path =
            ParseInstallPath(destination_value.text);
        const std::filesystem::path browse_initial =
            destination_path.value_or(status.default_destination);
        const bool create_desktop_shortcut =
            desktop_shortcut.Get().value_or(status.default_create_desktop_shortcut);
        eyebrow = installer_strings::installation;
        heading = installer_strings::ready_to_install;
        details.push_back(Text(installer_strings::installation_options)
                              .With(Foreground(theme.colors.on_surface_variant)));
        details.push_back(Row{TextField(destination_value)
                                  .Label(installer_strings::installation_folder)
                                  .Variant(TextFieldVariant::Outlined)
                                  .Validation(destination_path
                                                  ? ValidationResult::None()
                                                  : ValidationResult::Invalid(
                                                        installer_strings::invalid_installation_folder))
                                  .OnChanged([destination](const TextEditingValue& value) { destination = value; })
                                  .With(Grow()),
                              SecondaryAction(
                                  Button(installer_strings::browse)
                                      .OnClick([installer, destination, browse_initial, tasks] {
                                          tasks.Launch(
                                              [installer, destination, browse_initial]() -> Task<void> {
                                                  const std::optional<std::filesystem::path> selected =
                                                      co_await installer.ChooseDestinationAsync(browse_initial);
                                                  if (selected) {
                                                      destination =
                                                          TextEditingValue::FromText(InstallPathText(*selected));
                                                  }
                                              });
                                      }),
                                  theme)}
                                 .With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)));
        Color option_border = theme.colors.outline;
        option_border.alpha = 0.18F;
        details.push_back(
            Checkbox(installer_strings::create_desktop_shortcut, create_desktop_shortcut)
                .OnChanged([desktop_shortcut](bool enabled) { desktop_shortcut = enabled; })
                .With(Padding(EdgeInsets::Symmetric(14.0F, 10.0F)),
                      Background(theme.colors.surface_container_low), Border{.color = option_border},
                      CornerRadius(theme.shapes.medium)));
        actions.push_back(
            SecondaryAction(Button(installer_strings::cancel).OnClick([window] { window.Close(); }), theme));
        actions.push_back(Button(installer_strings::install)
                              .OnClick([installer, destination_path, create_desktop_shortcut] {
                                  if (destination_path) {
                                      installer.Install({.destination = *destination_path,
                                                         .create_desktop_shortcut = create_desktop_shortcut});
                                  }
                              })
                              .With(Enabled(destination_path.has_value()), Frame{.min_width = 112.0F}));
    } else if (status.phase == InstallerPhase::Ready) {
        // 已注册版本无法判定（未知状态码）：兜底说明页。
        eyebrow = installer_strings::setup;
        heading = installer_strings::setup_could_not_continue;
        panel_alignment = MainAxisAlignment::Center;
        details.push_back(Text(installer_strings::installed_version_unknown)
                              .With(Foreground(theme.colors.on_surface_variant)));
        actions.push_back(
            Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F}));
    } else if (status.phase == InstallerPhase::Planning || status.phase == InstallerPhase::Applying ||
               status.phase == InstallerPhase::Canceling) {
        eyebrow = installer_strings::installing;
        heading = installer_strings::installing_product;
        panel_alignment = MainAxisAlignment::Center;
        if (status.phase == InstallerPhase::Canceling) {
            eyebrow = installer_strings::rollback;
            heading = installer_strings::canceling_and_rolling_back;
        } else if (status.action == InstallerAction::Repair) {
            eyebrow = installer_strings::repairing;
            heading = installer_strings::repairing_product;
        } else if (status.action == InstallerAction::Uninstall) {
            eyebrow = installer_strings::removing;
            heading = installer_strings::uninstalling_product;
        }
        StringVariant progress_message =
            status.current_package.empty()
                ? StringVariant(installer_strings::operation_may_take_a_moment)
                : StringVariant(status.current_package);
        details.push_back(Text(std::move(progress_message))
                              .With(Foreground(theme.colors.on_surface_variant)));
        details.push_back(ProgressBar(status.progress));
        actions.push_back(SecondaryAction(
            Button(installer_strings::cancel).OnClick([installer] { installer.Cancel(); }), theme));
    } else if (status.phase == InstallerPhase::Completed) {
        eyebrow = installer_strings::complete;
        heading = status.action == InstallerAction::Uninstall ? installer_strings::application_removed
                                                              : installer_strings::installation_complete;
        panel_alignment = MainAxisAlignment::Center;
        StringVariant message;
        if (status.action == InstallerAction::Uninstall) {
            message = status.restart == InstallerRestart::Required
                          ? installer_strings::application_removed_restart_message
                          : installer_strings::application_removed_message;
        } else {
            message = status.restart == InstallerRestart::Required
                          ? installer_strings::installation_complete_restart_message
                          : installer_strings::installation_complete_message;
        }
        details.push_back(Row{CompletionMark(theme)}.With(MainAlign(MainAxisAlignment::Center)));
        details.push_back(Text(std::move(message)).With(Foreground(theme.colors.on_surface_variant)));
        actions.push_back(
            Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F}));
    } else if (status.phase == InstallerPhase::Canceled) {
        eyebrow = installer_strings::canceled;
        heading = installer_strings::installation_canceled;
        panel_alignment = MainAxisAlignment::Center;
        details.push_back(Text(installer_strings::no_further_changes)
                              .With(Foreground(theme.colors.on_surface_variant)));
        actions.push_back(
            Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F}));
    } else if (status.failure) {
        eyebrow = installer_strings::setup_error;
        heading = installer_strings::installation_failed;
        panel_alignment = MainAxisAlignment::Center;
        details.push_back(Text(status.failure->message).With(Foreground(theme.colors.error)));
        actions.push_back(
            Button(installer_strings::close).OnClick([window] { window.Close(); }).With(Frame{.min_width = 112.0F}));
    }

    std::vector<View> panel{Text(std::move(eyebrow))
                                .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold),
                                                 theme.colors.primary}),
                            Text(std::move(heading), TextRole::Title).With(FontSize(theme.typography.headline_small))};
    panel.insert(panel.end(), std::make_move_iterator(details.begin()), std::make_move_iterator(details.end()));

    std::vector<View> content;
    content.push_back(Column(std::move(panel))
                          .With(Spacing(16.0F), MainAlign(panel_alignment),
                                CrossAlign(CrossAxisAlignment::Stretch), Grow()));
    if (!actions.empty()) {
        content.push_back(Divider());
        content.push_back(Row(std::move(actions))
                              .With(Spacing(10.0F), MainAlign(MainAxisAlignment::End),
                                    CrossAlign(CrossAxisAlignment::Center)));
    }

    return Row{BrandPanel(theme),
               Column(std::move(content))
                   .With(Padding(EdgeInsets::Symmetric(36.0F, 32.0F)), Spacing(24.0F),
                         CrossAlign(CrossAxisAlignment::Stretch), Grow())}
        .With(CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.surface), Grow());
}

[[huxerui::composable]]
View InstallerPage() {
    // 自定义标题栏与主程序同一套 Custom chrome：品牌行落进标题栏，右侧
    // ─▢✕ 由框架渲染在系统按钮保留区。空白区必须由真实子项占住并标记
    // 拖拽（WindowDragRegion），否则命中测试落不到空 Row 的空隙上。
    const ThemeSpec theme = InstallerThemeSpec();
    return MaterialTheme(theme, InstallerThemeStyles(Column{
        WindowTitleBar{Text("APITAB / WINDOWS SETUP")
                           .Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold),
                                            theme.colors.primary})
                           .With(Padding(EdgeInsets::Symmetric(16.0F, 0.0F)), WindowDragRegion{}),
                       Spacer().With(Grow(1.0F), WindowDragRegion{})},
        InstallerContent(),
    }.With(Grow(), CrossAlign(CrossAxisAlignment::Stretch))));
}

} // namespace

const Application application{
    InstallerPage,
    {.window = {.title = "apitab Setup",
                // 标题栏占用客户区高度：初始/最小尺寸在内容尺寸上各让出 40pt。
                .initial_size = {780.0F, 540.0F},
                .minimum_size = Size{720.0F, 460.0F},
                .chrome_mode = WindowChromeMode::Custom,
                .title_bar_height = 40.0F},
     .show_debug_overlay = false,
     .window_hooks = {InstallInstallerSession}},
};
