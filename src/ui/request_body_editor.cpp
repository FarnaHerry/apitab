// request_body_editor.cpp — 请求 Body 文本编辑器与格式化 helpers。
// 自 request_page.cpp 拆出（P1-C1，功能域 = Body 编辑），纯搬移。
#include <huxerui/huxerui.h>
#include <sweetedit_core/sweet_editor.h>

#include <stdexcept>
#include <string>
#include <string_view>

#include "draft.h"
#include "syntax_grammars.h"
#include "ui.h"

import apitab.utils;

namespace apitab::ui {

// 各 body 类型的 SweetLine 语法：JSON/XML/GraphQL 高亮，其余纯文本。
std::string_view SyntaxForBodyKind(std::size_t kind) {
    switch (kind) {
        case 1: return kJsonSyntax;    // Json
        case 5: return kXmlSyntax;     // Xml
        case 6: return kGraphqlSyntax; // GraphQL
        default: return kPlainSyntax;  // Text 等
    }
}

// Body 文本编辑器：SweetEditor 代码编辑器（行号/语法高亮/等宽度量），固定高度
// 不随内容长高，超长行横向滚动（wrap_mode=0），由所在分区的 ScrollView 统一
// 垂直滚动。SweetEditor 非受控：只有 document_key 变化才重载 initial_text，
// 格式化等外部改文本由父级持同一 controller 走 LoadDocument 刷新（见
// RequestEditor 的格式化按钮）。配色经本地补丁的 palette 跟随主题（EditorPalette）。
[[huxerui::composable]] huxerui::View BodyTextEditor(
    huxerui::State<std::vector<RequestDraft>> drafts, std::size_t index,
    const RequestDraft& snapshot, const huxerui::ThemeSpec& theme,
    sweetedit_huxer::SweetEditorController controller) {
    const std::size_t kind = snapshot.bodyKindIndex;
    sweetedit_huxer::SweetEditorOptions options;
    options.palette = EditorPalette(theme);
    // document_key 绑定草稿 uid + body 类型：切请求或切 body 类型都会重载该
    // 类型自己存档的文本（各类型编辑框独立，输入不互通）。
    options.document_key = "body-" + std::to_string(snapshot.uid) + "-" +
                           std::to_string(kind);
    options.initial_text = snapshot.bodies[kind].text;
    options.syntax_json = std::string{SyntaxForBodyKind(kind)};
    options.wrap_mode = 0; // 不换行，横向滚动
    options.sticky_gutter = true; // 横向滚动时行号栏固定
    return sweetedit_huxer::SweetEditor(options, controller)
        .On<sweetedit_huxer::SweetEditorTextChanged>([drafts, index, controller, kind] {
            MutateDraft(drafts, index, [&](RequestDraft& d) {
                d.bodies[kind] = huxerui::TextEditingValue::FromText(controller.Text());
            });
        })
        .With(huxerui::Frame{.height = 220.0F}, huxerui::Grow(1.0F));
}

// 剥 JSON 注释（// 与 /* */；字符串内的不剥）。编辑区允许带注释的 JSON
// （allowJsonComments 语义），格式化前先剥掉再交给 nlohmann 解析。
std::string StripJsonComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool inString = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) out += in[++i];
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; out += c; continue; }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '/') {
            while (i < in.size() && in[i] != '\n') ++i;
            if (i < in.size()) out += '\n';
            continue;
        }
        if (c == '/' && i + 1 < in.size() && in[i + 1] == '*') {
            i += 2;
            while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) ++i;
            ++i; // 跳过结尾 '/'
            continue;
        }
        out += c;
    }
    return out;
}

// XML 美化：标签换行 + 每层两空格缩进；声明/注释/自闭合标签不增减层级，
// 文本节点独行。标签配对不完整时抛异常（由调用方转 toast）。
std::string PrettyXml(const std::string& input) {
    std::string out;
    int depth = 0;
    bool first = true;
    auto emit = [&](std::string_view piece, int d) {
        if (!first) out += '\n';
        first = false;
        out.append(static_cast<std::size_t>(std::max(d, 0)) * 2, ' ');
        out += piece;
    };
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t lt = input.find('<', pos);
        const std::string text = trim(input.substr(pos, lt == std::string::npos
                                                         ? std::string::npos
                                                         : lt - pos));
        if (!text.empty()) emit(text, depth);
        if (lt == std::string::npos) break;
        const std::size_t gt = input.find('>', lt);
        if (gt == std::string::npos) throw std::runtime_error("XML 标签未闭合");
        const std::string tag = input.substr(lt, gt - lt + 1);
        if (tag.starts_with("</")) {
            emit(tag, --depth);
        } else if (tag.ends_with("/>") || tag.starts_with("<?") || tag.starts_with("<!")) {
            emit(tag, depth);
        } else {
            emit(tag, depth++);
        }
        pos = gt + 1;
    }
    if (depth != 0) throw std::runtime_error("XML 标签配对不完整");
    return out;
}

} // namespace apitab::ui
