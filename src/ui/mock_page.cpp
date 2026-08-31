// mock_page.cpp — 请求编辑器子页"Mock"（pageTab=3）。
// 桩实现：待完整功能替换（模拟响应定义；调试页发送拦截另行接线）。
#include <huxerui/huxerui.h>

#include <vector>

#include "ui.h"

namespace apitab::ui {

[[huxerui::composable]] huxerui::View MockPage(RequestDraft snapshot,
                                               huxerui::State<std::vector<RequestDraft>> drafts,
                                               std::size_t index) {
    (void)snapshot;
    (void)drafts;
    (void)index;
    return huxerui::Text("Mock：建设中", huxerui::TextRole::Body);
}

} // namespace apitab::ui
