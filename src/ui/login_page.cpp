#include <huxerui/huxerui.h>

#include <string>

#include "app_resources.h"
#include "ui.h"

namespace apitab::ui {

[[huxerui::composable]] huxerui::View LoginPage(huxerui::DialogContext ctx,
                                                huxerui::State<bool> loggedIn) {
    const auto& theme = huxerui::UseTheme();
    auto account = huxerui::UseState(huxerui::TextEditingValue{});
    auto password = huxerui::UseState(huxerui::TextEditingValue{});
    auto passwordVisible = huxerui::UseState(false);
    auto passwordFieldHovered = huxerui::UseState(false);
    const bool passwordIsVisible = passwordVisible.Get();
    const bool showPasswordVisibilityToggle = passwordFieldHovered.Get();
    auto error = huxerui::UseState(std::string{});
    auto tasks = huxerui::UseTaskScope();

    auto submit = [account, password, error, loggedIn, tasks, ctx] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            if (account.Get().text == "admin" && password.Get().text == "admin") {
                error = {};
                loggedIn = true;
                ctx.Dismiss();
            } else {
                error = "账号或密码错误（演示账号：admin / admin）";
            }
        });
    };

    const auto& passwordVisibilityIcon = passwordIsVisible ? app::images::visibility_off
                                                           : app::images::visibility;
    const char* passwordVisibilityLabel = passwordIsVisible ? "隐藏密码" : "显示密码";
    auto passwordField = huxerui::TextField(password.Get())
                             .Secure(!passwordIsVisible)
                             .Label("密码")
                             .Placeholder("密码")
                             .Variant(huxerui::TextFieldVariant::Outlined)
                             .OnChanged([password](const huxerui::TextEditingValue& value) { password = value; })
                             .With(huxerui::Frame{.height = 48.0F});
    // 尾部动作只在整个密码框悬停时进入布局；离开字段即隐藏，避免常态视觉干扰。
    if (showPasswordVisibilityToggle) {
        passwordField = std::move(passwordField)
                            .TrailingIcon(passwordVisibilityIcon, passwordVisibilityLabel)
                            .OnTrailingIconClick([passwordVisible] {
                                passwordVisible = !passwordVisible.Get();
                            });
    }
    passwordField = std::move(passwordField).On<huxerui::ViewEvents::Hover>(
        [passwordFieldHovered](const huxerui::HoverEvent& event) {
            passwordFieldHovered = event.type != huxerui::HoverEventType::Leave;
        });

    huxerui::View form = huxerui::Column{
        huxerui::Text("欢迎回来", huxerui::TextRole::Title),
        huxerui::Text("登录 apitab，继续管理你的 API 项目", huxerui::TextRole::Body),
        huxerui::TextField(account.Get())
            .Label("账号")
            .Placeholder("账号")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([account](const huxerui::TextEditingValue& value) { account = value; })
            .With(huxerui::Frame{.height = 48.0F}),
        std::move(passwordField),
        error.Get().empty() ? huxerui::View{huxerui::Row{}}
                            : huxerui::View{huxerui::Text(error.Get(), huxerui::TextRole::Body)
                                                .With(huxerui::Foreground(theme.colors.error))},
        huxerui::Button("登录").OnClick(submit).With(huxerui::Frame{.height = 40.0F}),
    };

    return DialogCard(std::move(form).With(huxerui::Spacing(16.0F),
                                           huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                                           huxerui::Frame{.width = 380.0F}));
}

} // namespace apitab::ui
