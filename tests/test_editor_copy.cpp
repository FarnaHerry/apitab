// test_editor_copy — 响应区只读 CodeEditor 的运行时复现：指针拖选 + Ctrl+C 必须落到剪贴板。
// 用户报告：响应正文可以选中但复制不生效（Ctrl+C 与右键"复制"都无效）。
// 本测试用 HuxerUI fork 的测试支持（TestPlatform + 假剪贴板）复刻该链路，定位断点。
#include "runtime_test_support.h"

#include <huxerui/codeeditor.h>

#include <cstdio>
#include <functional>
#include <optional>
#include <string>

namespace {

using huxerui::test::TestPlatform;
using huxerui::test::UiWindow;

std::function<huxerui::View()> editor_test_root;
huxerui::View EditorCopyApp() { return editor_test_root(); }

class FakeClipboard final : public huxerui::PlatformClipboard {
public:
    std::optional<std::string> ReadText() override { return text; }

    bool WriteText(std::string_view value) override {
        text = std::string(value);
        return true;
    }

    std::optional<std::string> text;
};

void Pointer(UiWindow& runtime, huxerui::PointerEventType type, float x, float y) {
    runtime.HandlePointerEvent({type, 810, {x, y}});
}

// 与框架 selection_area 测试同款：递归查节点树上暴露的 TextSelectionClient。
huxerui::TextSelectionClient* FindClient(const huxerui::detail::MountedNode* node) {
    if (node == nullptr) return nullptr;
    for (const auto& entry : node->extensions) {
        if (entry.extension != nullptr)
            if (auto* client = entry.extension->GetTextSelectionClient()) return client;
    }
    for (const auto& child : node->children)
        if (auto* client = FindClient(child.get())) return client;
    return nullptr;
}

int failures = 0;

void Expect(bool condition, const char* what) {
    std::printf("[%-4s] %s\n", condition ? "OK" : "FAIL", what);
    if (!condition) ++failures;
}

} // namespace

int main() {
    // 与响应区 ResponseBody 同形态：read_only + 固定 document_key + 初始文本。
    editor_test_root = [] {
        huxerui::codeeditor::EditorOptions options;
        options.read_only = true;
        options.document_key = "response";
        options.initial_text = "Alpha\nBeta";
        auto controller = huxerui::codeeditor::UseEditorController();
        return huxerui::codeeditor::CodeEditor(options, controller);
    };

    TestPlatform platform;
    FakeClipboard clipboard;
    platform.platform_clipboard = &clipboard;
    UiWindow runtime{EditorCopyApp, platform};
    runtime.SetWindowMetrics({.viewport = {800.0F, 600.0F}});
    runtime.BuildFrame();

    // 拖选首行前若干字符（框架/组件内选择手势），随后直接询问复制能力。
    Pointer(runtime, huxerui::PointerEventType::Down, 10.0F, 10.0F);
    Pointer(runtime, huxerui::PointerEventType::Move, 300.0F, 12.0F);
    Pointer(runtime, huxerui::PointerEventType::Up, 300.0F, 12.0F);
    runtime.BuildFrame();

    const bool can_copy = runtime.CanPerformTextEditingAction(huxerui::TextEditingAction::Copy);
    const bool performed = can_copy && runtime.PerformTextEditingAction(huxerui::TextEditingAction::Copy);
    // 探测：client 是否挂在节点树上（存在则断点在焦点路由，不存在则 extension 未建）。
    const auto* tree_root = runtime.RootNode();
    huxerui::TextSelectionClient* tree_client = FindClient(tree_root);
    Expect(tree_client != nullptr, "a TextSelectionClient is mounted in the node tree");
    Expect(can_copy, "CanPerformTextEditingAction(Copy) after drag-select");
    Expect(performed, "PerformTextEditingAction(Copy) via selection client");
    Expect(clipboard.text.has_value() && *clipboard.text == "Alpha",
           "direct dispatch wrote selection to clipboard");

    // 用户路径：同一选中状态下按 Ctrl+C，剪贴板应被写入。
    clipboard.text.reset();
    runtime.HandleKeyEvent(
        {huxerui::KeyEventType::Down, huxerui::Key::C, {}, {.control = true}});
    runtime.BuildFrame();
    Expect(clipboard.text.has_value(), "Ctrl+C wrote selection to clipboard");
    std::printf("clipboard = %s\n",
                clipboard.text ? clipboard.text->c_str() : "(empty)");

    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
