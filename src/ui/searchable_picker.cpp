// searchable_picker.cpp — 作者推荐的可搜索组合框。
#include <huxerui/huxerui.h>

#include <cctype>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#include "ui.h"

namespace apitab::ui {

namespace {

bool MatchesSearch(const std::string& label, const std::string& query) {
    if (query.empty()) return true;
    std::string candidate = label;
    std::string needle = query;
    const auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::ranges::transform(candidate, candidate.begin(), lower);
    std::ranges::transform(needle, needle.begin(), lower);
    return candidate.find(needle) != std::string::npos;
}

} // namespace

// 作者推荐的受控可搜索组合框。selected_id 负责闭合态显示的选中项，
// onSelected 仅在业务需要副作用（例如切换当前环境）时使用。
[[huxerui::composable]] huxerui::View SearchablePicker(
    const std::vector<SearchItem>& items,
    huxerui::State<std::optional<std::string>> selected_id,
    huxerui::ImageVariant dropdown_icon, huxerui::ImageVariant search_icon,
    std::function<void(const std::string&)> onSelected,
    bool clear_selection_on_search) {
    auto query = huxerui::UseState(huxerui::TextEditingValue::FromText(""));
    auto expanded = huxerui::UseState(false);
    auto searchPlaceholder = huxerui::UseState(std::string{});
    // Opening the popup changes the controlled value on the next composition.  Keep a
    // one-shot guard so a platform text-input synchronization callback carrying the
    // previous closed value cannot immediately put that value back into the field.
    auto clearPending = huxerui::UseState(false);

    const std::optional<std::string> current_selected_id = selected_id;
    const huxerui::TextEditingValue current_query = query;
    const bool is_expanded = expanded;

    auto selected = items.end();
    if (current_selected_id) {
        selected = std::ranges::find(items, *current_selected_id, &SearchItem::id);
    }
    const std::string selected_label = selected == items.end() ? "" : selected->label;

    std::vector<SearchItem> visible_items;
    if (is_expanded) {
        std::ranges::copy_if(items, std::back_inserter(visible_items), [&](const SearchItem& item) {
            return MatchesSearch(item.label, current_query.text);
        });
    } else {
        visible_items = items;
    }

    const huxerui::TextEditingValue field_value =
        is_expanded ? current_query : huxerui::TextEditingValue::FromText(selected_label);
    const std::string activePlaceholder = clear_selection_on_search
                                              ? searchPlaceholder.Get()
                                              : selected_label;
    const huxerui::StringVariant placeholder =
        is_expanded && !activePlaceholder.empty() ? huxerui::StringVariant(activePlaceholder)
                                                  : huxerui::StringVariant("搜索环境");

    return huxerui::ComboBox(
               field_value, visible_items,
               [](const SearchItem& item) { return item.label; },
               [](const SearchItem& item) { return huxerui::Text(item.label).Key(item.id); })
        .Placeholder(placeholder)
        .Variant(huxerui::TextFieldVariant::Outlined)
        .TrailingIcon(is_expanded ? search_icon : dropdown_icon)
        .EmptyContent([] {
            return huxerui::Text("没有匹配的环境", huxerui::TextRole::Label)
                .With(huxerui::Padding(10.0F));
        })
        .OnChanged([query, clearPending, clear_selection_on_search, selected_label](
                       const huxerui::TextEditingValue& value) {
            if (clear_selection_on_search && clearPending.Get()) {
                clearPending = false;
                if (value.text == selected_label) {
                    query = huxerui::TextEditingValue::FromText("");
                    return;
                }
            }
            query = value;
        })
        .OnSelected([selected_id, visible_items, onSelected](
                        std::size_t index, const huxerui::TextEditingValue&) {
            if (index >= visible_items.size()) return;
            selected_id = visible_items[index].id;
            if (onSelected) onSelected(visible_items[index].id);
        })
        .OnExpandedChanged([expanded, query, searchPlaceholder,
                            clearPending, selected_label, clear_selection_on_search](bool open) {
            if (open) {
                query = huxerui::TextEditingValue::FromText("");
                if (clear_selection_on_search) {
                    // 搜索态不保留闭合态输入值：把原选中项转成占位符，
                    // 让用户直接输入查询词；底层 selected_id 保留，闭合态仍能稳定回显。
                    searchPlaceholder = selected_label;
                    clearPending = true;
                }
            } else {
                searchPlaceholder = std::string{};
                clearPending = false;
            }
            expanded = open;
        });
}

} // namespace apitab::ui
