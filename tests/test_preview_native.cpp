#include <huxerui/huxerui.h>
#include <huxerui/webview.h>

#include "ui/page_card.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {
using namespace huxerui;
using namespace std::chrono_literals;

bool passed = false;
bool lightTheme = false;
constexpr char kDocumentUrl[] = "https://apitab-preview.test/";

View PreviewTestRoot() {
    const auto stage = UseState(0);
    const auto tasks = UseTaskScope();
    const auto app = UseApplication();
    Lifecycle([stage, tasks, app] {
        tasks.Launch([stage]() -> Task<void> {
            co_await Delay(100ms);
            stage = 1;
        });
        tasks.Launch([app]() -> Task<void> {
            co_await Delay(8s);
            std::fprintf(stderr, "Preview HTML load timed out\n");
            app.Quit();
        });
    }, 0);

    View content = Scope([stage, tasks, app]() -> View {
        if (stage.Get() % 2 == 0) return Text("Body");
        return Scope([stage, tasks, app] {
            const auto controller = UseWebViewController();
            const auto allowInitialNavigation = UseState(true);
            const auto finished = UseState(false);
            Lifecycle([controller] {
                controller.LoadHtml({
                    .content = "<!doctype html><html><head><title>apitab Preview regression</title>"
                               "</head><body><h1>HTML response</h1></body></html>",
                    .base_url = kDocumentUrl,
                });
            }, 0);
            return WebView({.url = "about:blank", .java_script_enabled = true}, controller)
                .On<WebViewEvents::NavigationRequested>([allowInitialNavigation](const WebViewNavigationRequest& request) {
                    if (!request.is_main_frame || request.url == "about:blank") return true;
                    if (!allowInitialNavigation.Get()) return false;
                    allowInitialNavigation = false;
                    return true;
                })
                .On<WebViewEvents::LoadFinished>([stage, tasks, app, finished](const WebViewNavigationState& state) {
                    // about:blank 也可能发 LoadFinished，必须等真正的 HTML 文档。
                    if (state.url != kDocumentUrl || finished.Get()) return;
                    finished = true;
                    std::fprintf(stderr, "Preview HTML loaded, mount %d\n", (stage.Get() + 1) / 2);
                    tasks.Launch([stage, app]() -> Task<void> {
                        // 不在原生回调栈内销毁控件。首次加载后卸载再挂载，覆盖切换标签。
                        co_await Delay(100ms);
                        if (stage.Get() == 1) {
                            stage = 2;
                            co_await Delay(100ms);
                            stage = 3;
                        } else {
                            passed = true;
                            app.Quit();
                        }
                    });
                })
                .With(Grow(1.0F), ClipChildren());
        }).Key("preview");
    });
    auto theme = lightTheme ? MaterialLightThemeSpec() : MaterialDarkThemeSpec();
    theme.shapes.large = 10.0F;
    return apitab::ui::PageCardSurface(content, theme, Color::Rgb(208, 215, 222));
}

const Application application{PreviewTestRoot, {
    .window = {.title = "apitab Preview regression", .initial_size = {600.0F, 400.0F}},
    .application_hooks = {InstallWebView},
}};
} // namespace

int main(int argc, char**) {
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY")) return 77;
    lightTheme = argc > 1;
    try {
        const int result = RunApplication();
        return result != 0 ? result : (passed ? 0 : 1);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Preview native regression: %s\n", error.what());
        return 1;
    }
}
