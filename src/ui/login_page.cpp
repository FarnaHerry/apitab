#include <huxerui/huxerui.h>

#include <string>

#include "ui.h"

namespace apitab::ui {

[[huxerui::composable]] huxerui::View LoginPage(huxerui::DialogContext ctx,
                                                huxerui::State<bool> loggedIn) {
    const auto& theme = huxerui::UseTheme();
    auto account = huxerui::UseState(huxerui::TextEditingValue{});
    auto password = huxerui::UseState(huxerui::TextEditingValue{});
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

    huxerui::View form = huxerui::Column{
        huxerui::Text("欢迎回来", huxerui::TextRole::Title),
        huxerui::Text("登录 apitab，继续管理你的 API 项目", huxerui::TextRole::Body),
        huxerui::TextField(account.Get())
            .Label("账号")
            .Placeholder("账号")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([account](const huxerui::TextEditingValue& value) { account = value; })
            .With(huxerui::Frame{.height = 48.0F}),
        huxerui::TextField(password.Get())
            .Label("密码")
            .Placeholder("密码")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([password](const huxerui::TextEditingValue& value) { password = value; })
            .With(huxerui::Frame{.height = 48.0F}),
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
