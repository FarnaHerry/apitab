// testcase_page.cpp — 请求编辑器子页"测试用例"（pageTab=2）。
// 桩实现：待完整功能替换（用例编辑 + 运行断言 + 结果展示）。
#include <huxerui/huxerui.h>

#include <vector>

#include "ui.h"

namespace apitab::ui {

[[huxerui::composable]] huxerui::View TestCasePage(RequestDraft snapshot,
                                                   huxerui::State<std::vector<RequestDraft>> drafts,
                                                   std::size_t index) {
    (void)snapshot;
    (void)drafts;
    (void)index;
    return huxerui::Text("测试用例：建设中", huxerui::TextRole::Body);
}

} // namespace apitab::ui
