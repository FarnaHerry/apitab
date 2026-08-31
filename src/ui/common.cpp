// common.cpp — HuxerUI 前端公共小组件（页面标题、状态文本、空状态页、圆形按钮、
// 方法+地址组合栏）。下拉选择一律用官方 Select（view.h），不再自拼菜单触发器。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "app_resources.h"
#include "ui.h"

import std;

import apitab.utils;

namespace apitab::ui {

// 纯圆形单符号按钮：无文字标签（框架 Button 是长胶囊且有内建最小宽度，做不出
// 纯圆）。accent=true 主色底（主动作），false 中性容器底（次动作）。符号用
// Text 渲染，点击区 = 整个圆。
[[huxerui::composable]] huxerui::View CircleButton(std::string glyph,
                                                   std::function<void()> onClick, bool accent) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Row {
        huxerui::Text(std::move(glyph), huxerui::TextRole::Label)
            .With(huxerui::Foreground(accent ? theme.colors.on_primary
                                             : theme.colors.on_surface)),
    }
        .With(huxerui::Frame{.width = 28.0F, .height = 28.0F},
              huxerui::Background(accent ? theme.colors.primary
                                         : theme.colors.surface_container_highest),
              huxerui::CornerRadius(theme.shapes.full),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
        .OnClick(std::move(onClick));
}

// 自定义内容弹窗的卡片包裹：SDK 的 dialog.Show(ViewFactory/DialogFactory) 不给
// 内容加底板（只有标题+消息的内置形态才有 DialogStyle），裸内容直接浮在页面上
// 分不清层级。统一包一层：圆角底 + 阴影 + 内边距；阴影/底/圆角依次在外，
// Padding 在最内（modifier 顺序 = 由外到内）。
[[huxerui::composable]] huxerui::View DialogCard(huxerui::View content) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 作用域宏里参数以 const 引用形式可见、View::With 只能作用于右值：
    // 先拷出可变副本，再 std::move 挂修饰。
    huxerui::View card = content;
    return std::move(card).With(
        huxerui::Shadow{huxerui::Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F},
        huxerui::Background(theme.colors.surface_container_high),
        huxerui::CornerRadius(theme.shapes.large),
        huxerui::Padding(theme.spacing.large));
}

// 危险确认弹窗内容（ShowDangerConfirm 的 DialogFactory 目标）：标题 + 消息 +
// 按钮行（左取消右确认）。确认按钮染红走 ProvideEnvironment 局部覆盖 ButtonStyle
// ——Button 无单实例样式 API（样式经 ResolveStyleOverride<ButtonStyle> 解析），
// Environment 是最窄机制；ColorScheme 没有 on_error 令牌，红底上文字固定白色。
[[huxerui::composable]] huxerui::View DangerConfirmContent(
    huxerui::DialogContext ctx, std::string title, std::string message,
    std::string confirmLabel, std::function<void()> onConfirm) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    huxerui::ButtonStyle danger = huxerui::UseEnvironment<huxerui::ButtonStyle>();
    danger.background = theme.colors.error;
    danger.label_style.foreground = huxerui::Color::White();
    return DialogCard(huxerui::Column {
        huxerui::Text(std::move(title), huxerui::TextRole::Title),
        huxerui::Text(std::move(message), huxerui::TextRole::Body),
        huxerui::Row {
            huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
            huxerui::ProvideEnvironment(
                danger, huxerui::View{huxerui::Button(std::move(confirmLabel))
                                          .OnClick([ctx, onConfirm = std::move(onConfirm)] {
                                              ctx.Dismiss();
                                              onConfirm();
                                          })}),
        }
            .With(huxerui::Spacing(8.0F),
                  huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)),
    }
        .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 320.0F},
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

void ShowDangerConfirm(huxerui::DialogHandle dialog, std::string title, std::string message,
                       std::string confirmLabel, std::function<void()> onConfirm) {
    dialog.Show(
        [title = std::move(title), message = std::move(message),
         confirmLabel = std::move(confirmLabel),
         onConfirm = std::move(onConfirm)](huxerui::DialogContext ctx) -> huxerui::View {
            return DangerConfirmContent(ctx, title, message, confirmLabel, onConfirm);
        },
        huxerui::DialogOptions{});
}

// huxerui::Color（float RGBA）→ SweetEditor 调色板的 ARGB int32。
static std::int32_t ToArgb(huxerui::Color c) {
    auto channel = [](float v) {
        if (v < 0.0F) v = 0.0F;
        if (v > 1.0F) v = 1.0F;
        return static_cast<std::uint32_t>(v * 255.0F + 0.5F);
    };
    return static_cast<std::int32_t>((channel(c.alpha) << 24) | (channel(c.red) << 16) |
                                     (channel(c.green) << 8) | channel(c.blue));
}

sweetedit_huxer::SweetEditorPalette EditorPalette(const huxerui::ThemeSpec& theme) {
    sweetedit_huxer::SweetEditorPalette palette; // 默认值 = 上游浅色主题
    if (theme.colors.surface.red > 0.5F) return palette; // 浅色：沿用默认

    // 深色：结构色从 ThemeSpec 派生（编辑器嵌在 surface_container_low 岛上），
    // 语法/补全强调色取 VS Code Dark+ 近似值（Alpha 叠加色明暗通用，保持默认）。
    const auto& c = theme.colors;
    palette.editor_background = ToArgb(c.surface_container_low);
    palette.gutter_background = ToArgb(c.surface_container);
    palette.gutter_split_line = ToArgb(c.surface_container_high);
    palette.line_number = ToArgb(c.on_surface_variant);
    palette.cursor = ToArgb(c.on_surface);
    palette.text_foreground = ToArgb(c.on_surface);
    palette.separator = ToArgb(c.surface_container_highest);
    palette.gutter_icon = ToArgb(c.on_surface_variant);
    palette.completion_panel_background = ToArgb(c.surface_container_highest);
    palette.completion_label = ToArgb(c.on_surface);
    palette.completion_detail = ToArgb(c.on_surface_variant);
    palette.context_menu_background = ToArgb(c.surface_container_highest);
    palette.context_menu_item_text = ToArgb(c.on_surface);

    palette.selection_background = 0x55264F78;
    palette.selection_handle = 0xFF4C9DFF;
    palette.current_line_background = 0x0FFFFFFF;
    palette.guide = 0xFF3A3A42;
    palette.inlay_hint_text = 0xB09AA0A6;
    palette.fold_placeholder_text = 0xFFA9C7E8;
    palette.whitespace_marker = 0x40FFFFFF;
    palette.whitespace_symbol = 0x4DFFFFFF;
    palette.scrollbar_thumb = 0x73FFFFFF;
    palette.completion_panel_border = 0x30FFFFFF;
    palette.context_menu_border = 0xFF3A3A42;
    palette.active_codelens_foreground = 0xFF8AB4F8;

    palette.syntax_keyword = 0xFFC586C0;
    palette.syntax_type = 0xFF4EC9B0;
    palette.syntax_class = 0xFF4EC9B0;
    palette.syntax_function = 0xFFDCDCAA;
    palette.syntax_variable = 0xFF9CDCFE;
    palette.syntax_string = 0xFFCE9178;
    palette.syntax_number = 0xFFB5CEA8;
    palette.syntax_comment = 0xFF6A9955;
    palette.syntax_preprocessor = 0xFFC586C0;
    palette.syntax_builtin = 0xFF4FC1FF;
    palette.syntax_punctuation = 0xFFD4D4D4;
    palette.syntax_annotation = 0xFFD7BA7D;
    palette.syntax_url = 0xFF4C9DFF;
    return palette;
}

// 自绘弹出菜单内容（ShowPopupMenu[At] 的 PopupFactory 目标）。作者口径：通用
// MenuItem 不支持 per-item 配色/hover 定制，需要就自己用 UsePopup 做菜单内容。
// 外观对齐环境 MenuStyle（底板/阴影/圆角/内边距/最小宽、条目 hover 用
// item_indication）；选中项不用对钩，直接填充比 hover 深一档的底色（取
// item_indication 的 press 填充色，兜底 surface_container_highest）；危险项按
// PopupMenuDanger 取 error 红（kHoverRed 由逐条 Hover 事件驱动，仅 hover 时
// 变红）；点击先 Dismiss 再回调（同系统菜单语义，回调脱离指针路径，但仍需
// 自行推迟会卸载节点的 State 写）。
[[huxerui::composable]] huxerui::View PopupMenuContent(huxerui::PopupContext ctx,
                                                       std::vector<PopupMenuItem> items) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const huxerui::MenuStyle menuStyle = huxerui::UseEnvironment<huxerui::MenuStyle>();
    auto hovered = huxerui::UseState(-1); // 悬停条目下标（Hover 是包含生命周期）
    // 选中底色：比 hover 深一档 = 菜单样式的 press 填充。
    huxerui::Color selectedFill = theme.colors.surface_container_highest;
    if (menuStyle.item_indication.press.has_value() &&
        menuStyle.item_indication.press->fill.has_value()) {
        if (const huxerui::Color* c =
                std::get_if<huxerui::Color>(&menuStyle.item_indication.press->fill->Get()))
            selectedFill = *c;
    }
    std::vector<huxerui::View> rows;
    rows.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        const PopupMenuItem& item = items[i];
        const bool dangerRed =
            item.danger == PopupMenuDanger::kAlwaysRed ||
            (item.danger == PopupMenuDanger::kHoverRed && hovered.Get() == static_cast<int>(i));
        const huxerui::Color labelColor =
            item.label_color.has_value() ? *item.label_color
            : dangerRed                  ? theme.colors.error
                                         : menuStyle.foreground;
        rows.push_back(
            huxerui::Row {
                huxerui::Text(item.label, huxerui::TextRole::Body)
                    .With(huxerui::Foreground(labelColor)),
            }
                .With(// 选中项：深于 hover 的填充底色（无对钩）。Background 在
                      // Padding 外层，整行铺满。
                      huxerui::Background(item.checked ? selectedFill
                                                       : huxerui::Color::Transparent()),
                      huxerui::CornerRadius(menuStyle.corner_radius / 2.0F),
                      huxerui::Padding(menuStyle.item_padding),
                      huxerui::Frame{.min_height = menuStyle.minimum_item_height},
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      // 条目悬停/按压反馈复用菜单样式的 indication（OnClick
                      // 默认 indication 以此覆盖），叠在选中底色之上。
                      menuStyle.item_indication)
                .On<huxerui::ViewEvents::Hover>(
                    [hovered, i](const huxerui::HoverEvent& e) {
                        if (e.type == huxerui::HoverEventType::Enter)
                            hovered = static_cast<int>(i);
                        else if (e.type == huxerui::HoverEventType::Leave &&
                                 hovered.Get() == static_cast<int>(i))
                            hovered = -1;
                    })
                .OnClick([ctx, onClick = item.on_click] {
                    ctx.Dismiss();
                    if (onClick) onClick();
                }));
    }
    return huxerui::Column{std::move(rows)}.With(
        menuStyle.shadow, huxerui::Background(menuStyle.background),
        huxerui::CornerRadius(menuStyle.corner_radius), huxerui::Padding(menuStyle.content_padding),
        huxerui::Frame{.min_width = menuStyle.minimum_width},
        huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

void ShowPopupMenu(huxerui::PopupHandle popup, std::vector<PopupMenuItem> items,
                   const huxerui::PopupOptions& options) {
    popup.Show(
        [items = std::move(items)](huxerui::PopupContext ctx) {
            return PopupMenuContent(ctx, items);
        },
        options);
}

void ShowPopupMenuAt(huxerui::PopupHandle popup, huxerui::Point point,
                     std::vector<PopupMenuItem> items, const huxerui::PopupOptions& options) {
    popup.ShowAt(
        point,
        [items = std::move(items)](huxerui::PopupContext ctx) {
            return PopupMenuContent(ctx, items);
        },
        options);
}

// 方法 + URL 合并控件（Postman 风格）：左侧扁平方法选择（文本 ▾ 弹自绘下拉，
// DELETE 常驻红；保持自绘而非官方 Select——Select 触发器自带描边外观，塞不进
// 这个共用外框的组合栏），其后是当前环境 baseUrl 显示区（灰色只读、截断；
// 输入框内容带 URI scheme 时以输入为准不拼接 → 该段半透明弱化），中间 1pt
// 分隔线，右侧 URL 输入；整体共用一个描边圆角外框。URL 字段经
// ProvideEnvironment 局部覆盖 TextFieldStyle：透明描边 + 零圆角，边框完全交给
// 外框（TextField 无单实例样式 API，Environment 是最窄机制）。
[[huxerui::composable]] huxerui::View MethodUrlBar(
    std::vector<std::string> methods, std::size_t methodIndex,
    std::function<void(std::size_t)> onMethodChanged, huxerui::TextEditingValue url,
    std::function<void(const huxerui::TextEditingValue&)> onUrlChanged,
    std::string baseUrl, std::string placeholder) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto popup = huxerui::UsePopup(); // 方法下拉：自绘内容（DELETE 常驻红，MenuItem 无文字配色 API）
    const std::size_t safe = methodIndex < methods.size() ? methodIndex : 0;

    huxerui::TextFieldStyle urlStyle = huxerui::UseEnvironment<huxerui::TextFieldStyle>();
    urlStyle.outlined.border = huxerui::Color::Transparent();
    urlStyle.outlined.hovered_border = huxerui::Color::Transparent();
    urlStyle.outlined.focused_border = huxerui::Color::Transparent();
    urlStyle.corner_radius = 0.0F;
    // 高度对齐旁边的"发送"按钮：ButtonStyle 默认 minimum_height=0，按钮高由内容
    // 撑出 = 14pt 文字行高（约 16pt）+ padding Symmetric(14,8) 的 16pt ≈ 32pt；
    // 而 Outlined 变体默认 minimum_height=36，整条控件比按钮高一截。这里把
    // minimum_height 收到 32，内容高（14pt 文字 + 垂直 padding 12pt ≈ 28pt）低于
    // 它，由 minimum_height 定高且文字垂直居中；padding 同步收 8→6 保持居中余量。
    urlStyle.outlined.minimum_height = 32.0F;
    urlStyle.padding = huxerui::EdgeInsets::Symmetric(10.0F, 6.0F);

    // 方法触发器：扁平文本，点击弹锚定 Popup（自绘下拉，外观对齐菜单层语义）。
    // 垂直内边距 8→6：与上面 URL 字段收到的 32pt 同高对齐（文字 14pt + 12pt ≈
    // 26pt < 32pt，外层 Row Stretch 拉满后由 CrossAlign(Center) 垂直居中）。
    // 触发器与下拉项文字都按 MethodColor 统一色表逐方法着色。
    huxerui::View trigger =
        huxerui::Row {
            huxerui::Text(methods.at(safe) + " ▾", huxerui::TextRole::Body)
                .With(huxerui::Foreground(MethodColor(theme, methods.at(safe)))),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(10.0F, 6.0F)),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center))
            .OnClick([popup, methods = std::move(methods), current = safe,
                      onChanged = std::move(onMethodChanged), &theme] {
                // 点击时组件仍挂载、theme 引用有效；颜色在弹层内容里随点击
                // 时刻烘焙（菜单打开期间切主题属极端情况，重开即刷新）。
                std::vector<PopupMenuItem> items;
                items.reserve(methods.size());
                for (std::size_t i = 0; i < methods.size(); ++i) {
                    items.push_back(PopupMenuItem{
                        .label = methods[i],
                        .on_click = [onChanged, i] { onChanged(i); },
                        .checked = i == current,
                        .label_color = MethodColor(theme, methods[i]),
                    });
                }
                ShowPopupMenu(popup, std::move(items),
                              huxerui::PopupOptions{
                                  .placement = {huxerui::AnchorSide::Below,
                                                huxerui::AnchorAlignment::Start}});
            });
    trigger = std::move(trigger).With(popup.Anchor());

    // baseUrl 显示区：当前环境基础 URL（灰色只读、max_width 截断）。输入框自带
    // URI scheme 时以输入为准（finalizeSpec 不拼接）→ 该段半透明弱化提示；
    // baseUrl 为空（无环境/未配置）时整段不渲染。
    huxerui::View baseSegment = huxerui::Row{};
    if (!baseUrl.empty()) {
        const bool overridden = hasUriScheme(trim(url.text));
        baseSegment = huxerui::Row {
            huxerui::Text(baseUrl, huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        }
            .With(huxerui::Padding(huxerui::EdgeInsets::Symmetric(8.0F, 0.0F)),
                  huxerui::Frame{.max_width = 160.0F}, huxerui::ClipChildren(),
                  // 外层 Row 是 Stretch（为分隔线拉满全高），本段需自行垂直居中。
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                  huxerui::Opacity(overridden ? 0.4F : 1.0F));
    }

    return huxerui::Row {
        std::move(trigger),
        std::move(baseSegment),
        // 分隔线：父 Row 交叉轴 Stretch 拉满全高。
        huxerui::Column{}.With(huxerui::Frame{.width = 1.0F},
                               huxerui::Background(theme.colors.outline)),
        huxerui::ProvideEnvironment(
            urlStyle,
            huxerui::View{huxerui::TextField(url)
                              .Placeholder(std::move(placeholder))
                              .Variant(huxerui::TextFieldVariant::Outlined)
                              .OnChanged(std::move(onUrlChanged))
                              .With(huxerui::Grow(1.0F))}),
    }
        .With(huxerui::Spacing(0.0F),
              huxerui::Border(theme.colors.outline, 1.0F),
              huxerui::CornerRadius(theme.shapes.small), huxerui::ClipChildren(),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

// 页面标题 + 副标题。
[[huxerui::composable]] huxerui::View PageHeader(std::string title, std::string subtitle) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
               huxerui::Text(std::move(title), huxerui::TextRole::Title),
               huxerui::Text(std::move(subtitle), huxerui::TextRole::Body),
           }
        .With(huxerui::Spacing(4.0F), huxerui::Foreground(theme.colors.on_surface_variant));
}

// 尚未迁移完成的页面占位。
[[huxerui::composable]] huxerui::View MigrationPlaceholder(std::string pageName) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
               huxerui::Text(std::move(pageName) + " — 迁移中", huxerui::TextRole::Title),
               huxerui::Text("此页面正在从 EUI-NEO 前端迁移到 HuxerUI，功能暂不可用。",
                             huxerui::TextRole::Body),
           }
        .With(huxerui::Padding(theme.spacing.large), huxerui::Spacing(8.0F),
              huxerui::Foreground(theme.colors.on_surface_variant));
}

// 空状态页：图标 + 标题 + 副标题，可选行动按钮（如“去主页打开项目”）。
// navPage 用来在按钮里推迟切页（事件处理器内禁止同步写会卸载节点的 State，
// CLAUDE.md 约定 6），由调用方传入；按钮通过 tasks.Launch + Delay(0) 推迟。
[[huxerui::composable]] huxerui::View EmptyState(huxerui::ImageResource icon,
                                                  std::string title, std::string subtitle,
                                                  huxerui::State<std::size_t> navPage) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    return huxerui::Column {
               huxerui::Image(icon)
                   .With(huxerui::Frame{.width = 64.0F, .height = 64.0F},
                         huxerui::Foreground(theme.colors.on_surface_variant)),
               huxerui::Text(std::move(title), huxerui::TextRole::Title),
               huxerui::Text(std::move(subtitle), huxerui::TextRole::Body)
                   .With(huxerui::Foreground(theme.colors.on_surface_variant)),
               huxerui::Button("去主页打开项目").OnClick([tasks, navPage] {
                   tasks.Launch([=]() -> huxerui::Task<void> {
                       co_await huxerui::Delay(std::chrono::duration<double>{0});
                       navPage = std::size_t{0};
                   });
               }),
           }
        .With(huxerui::Padding(theme.spacing.extra_large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::Foreground(theme.colors.on_surface_variant));
}

} // namespace apitab::ui
