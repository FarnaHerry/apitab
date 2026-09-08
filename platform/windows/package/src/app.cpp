#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/huxerui.h>
#include <huxerui/windows/installer.h>

using namespace huxerui;
using namespace huxerui::windows;

namespace {

std::string PathText(const std::filesystem::path& path) {
  const std::u8string value = path.u8string();
  return {value.begin(), value.end()};
}

std::optional<std::filesystem::path> AbsolutePath(std::string_view value) {
  if (value.empty()) return std::nullopt;
  try {
    std::filesystem::path path(std::u8string(value.begin(), value.end()));
    return path.is_absolute() ? std::optional<std::filesystem::path>{path.lexically_normal()} : std::nullopt;
  } catch (const std::filesystem::filesystem_error&) {
    return std::nullopt;
  }
}

[[huxerui::composable]]
View InstallerPage() {
  const InstallerHandle installer = UseInstaller();
  const InstallerStatus status = installer.Status();
  const WindowHandle window = UseWindow();
  const TaskScope tasks = UseTaskScope();
  const ThemeSpec& theme = UseTheme();
  auto destination = UseState<std::optional<TextEditingValue>>(std::nullopt);
  auto desktop_shortcut = UseState<std::optional<bool>>(std::nullopt);

  std::string title = "Preparing apitab setup";
  std::vector<View> content;
  std::vector<View> actions;

  if (status.prompt) {
    title = "Setup needs your attention";
    content.push_back(Text(status.prompt->message.empty() ? "Close applications using apitab, then continue."
                                                          : status.prompt->message));
    if (!status.prompt->choices.empty()) {
      const InstallerPromptChoice choice = status.prompt->recommended.value_or(status.prompt->choices.front());
      actions.push_back(Button("Continue").OnClick([installer, id = status.prompt->id, choice] {
        installer.Respond(id, choice);
      }));
    }
  } else if (status.phase == InstallerPhase::Detecting) {
    content.push_back(Text("Checking installed versions…").With(Foreground(theme.colors.on_surface_variant)));
    content.push_back(ProgressBar(status.progress));
  } else if (status.phase == InstallerPhase::Ready && status.product == InstallerProductState::Absent) {
    title = "Install apitab";
    const TextEditingValue value = destination.Get().value_or(TextEditingValue::FromText(PathText(status.default_destination)));
    const std::optional<std::filesystem::path> path = AbsolutePath(value.text);
    const bool shortcut = desktop_shortcut.Get().value_or(status.default_create_desktop_shortcut);
    content.push_back(Text("Choose an installation folder and optional desktop shortcut.")
                          .With(Foreground(theme.colors.on_surface_variant)));
    content.push_back(Row {
      TextField(value).Label("Installation folder").Variant(TextFieldVariant::Outlined)
          .Validation(path ? ValidationResult::None() : ValidationResult::Invalid("Enter an absolute path"))
          .OnChanged([destination](const TextEditingValue& next) { destination = next; }).With(Grow()),
      Button("Browse…").OnClick([installer, tasks, destination, initial = path.value_or(status.default_destination)] {
        tasks.Launch([installer, destination, initial]() -> Task<void> {
          if (const auto selected = co_await installer.ChooseDestinationAsync(initial)) {
            destination = TextEditingValue::FromText(PathText(*selected));
          }
        });
      }),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)));
    content.push_back(Checkbox("Create a desktop shortcut", shortcut)
                          .OnChanged([desktop_shortcut](bool value) { desktop_shortcut = value; }));
    actions.push_back(Button("Cancel").OnClick([window] { window.Close(); }));
    actions.push_back(Button("Install").OnClick([installer, path, shortcut] {
      if (path) installer.Install({.destination = *path, .create_desktop_shortcut = shortcut});
    }).With(Enabled(path.has_value())));
  } else if (status.phase == InstallerPhase::Ready && status.product == InstallerProductState::Present) {
    title = "apitab is already installed";
    content.push_back(Text("Repair the existing installation or uninstall it from this computer.")
                          .With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(Button("Uninstall").OnClick([installer] { installer.Uninstall(); }));
    actions.push_back(Button("Repair").OnClick([installer] { installer.Repair(); }));
  } else if (status.phase == InstallerPhase::Planning || status.phase == InstallerPhase::Applying ||
             status.phase == InstallerPhase::Canceling) {
    title = status.phase == InstallerPhase::Canceling ? "Canceling and rolling back" : "Installing apitab";
    content.push_back(Text(status.current_package.empty() ? "This may take a moment." : status.current_package)
                          .With(Foreground(theme.colors.on_surface_variant)));
    content.push_back(ProgressBar(status.progress));
    actions.push_back(Button("Cancel").OnClick([installer] { installer.Cancel(); }));
  } else if (status.phase == InstallerPhase::Completed) {
    title = status.action == InstallerAction::Uninstall ? "apitab removed" : "Installation complete";
    content.push_back(Text(status.restart == InstallerRestart::Required ? "Restart Windows to finish setup."
                                                                    : "You can now close this installer."));
    actions.push_back(Button("Close").OnClick([window] { window.Close(); }));
  } else if (status.phase == InstallerPhase::Failed) {
    title = "Installation failed";
    content.push_back(Text(status.failure ? status.failure->message : "Setup could not continue.")
                          .With(Foreground(theme.colors.error)));
    actions.push_back(Button("Close").OnClick([window] { window.Close(); }));
  } else {
    title = "Setup canceled";
    content.push_back(Text("No further changes will be made.").With(Foreground(theme.colors.on_surface_variant)));
    actions.push_back(Button("Close").OnClick([window] { window.Close(); }));
  }

  return MaterialTheme {Column {
    Text("APITAB / WINDOWS SETUP").Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::SemiBold), theme.colors.primary}),
    Text(std::move(title), TextRole::Title),
    Column(std::move(content)).With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch), Grow()),
    Divider(),
    Row(std::move(actions)).With(Spacing(10.0F), MainAlign(MainAxisAlignment::End)),
  }.With(Frame{.min_width = 620.0F, .min_height = 390.0F}, Padding(32.0F), Spacing(20.0F),
         CrossAlign(CrossAxisAlignment::Stretch), Background(theme.colors.surface))};
}

} // namespace

const Application application{
    InstallerPage,
    {.window = {.title = "apitab Setup", .initial_size = {720.0F, 470.0F}, .minimum_size = Size{620.0F, 390.0F}},
     .show_debug_overlay = false,
     .root_hooks = {InstallInstallerSession}},
};
