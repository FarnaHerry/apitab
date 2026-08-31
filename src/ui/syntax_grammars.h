// syntax_grammars.h — SweetLine 语法高亮定义（SweetEditor 的 syntax_json）。
// 上游 sweetedit 只附带 cpp/java/lua/kotlin 语法，JSON/JavaScript 由本仓库自带。
// 格式参考 third_party/huxerui-sweetedit/resources/raw/syntaxes/lua.json：
// fragments 定义可复用规则，states.default 里 {"include": "名字"} 引用；
// styles 数组按捕获组序号配样式（style 名单值等价于整体一个样式）。
#pragma once

#include <string_view>

namespace apitab::ui {

// JSON：字符串/数字/true-false-null/标点 + 注释（apitab 的 body 允许 JSON 注释，
// 见 StripJsonComments；高亮同步认可 // 与 /* */，避免注释被染成字符串色）。
inline constexpr std::string_view kJsonSyntax = R"JSON({
  "name": "json",
  "fileSuffixes": [".json"],
  "fragments": {
    "stringRule": [
      {"pattern": "(\")((?:[^\"\\\\]|\\\\.)*)(\")", "styles": [1, "punctuation", 2, "string", 3, "punctuation"]}
    ],
    "lineCommentRule": [
      {"pattern": "(//)(.*)", "styles": [1, "comment", 2, "comment"]}
    ],
    "blockCommentRule": [
      {"pattern": "(/\\*)([\\S\\s]*?)(\\*/)", "styles": [1, "comment", 2, "comment", 3, "comment"]}
    ]
  },
  "states": {
    "default": [
      {"include": "blockCommentRule"},
      {"include": "lineCommentRule"},
      {"include": "stringRule"},
      {"pattern": "-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?", "style": "number"},
      {"pattern": "\\b(?:true|false|null)\\b", "style": "keyword"},
      {"pattern": "[{}\\[\\],:]", "style": "punctuation"}
    ]
  },
  "scopeRules": {
    "skips": [
      {"kind": "blockComment", "start": "/*", "end": "*/"},
      {"kind": "lineComment", "start": "//"},
      {"kind": "string", "start": "\"", "end": "\"", "escape": "\\"}
    ]
  },
  "bracketRules": {"pairs": [{"start": "{", "end": "}"}, {"start": "[", "end": "]"}]}
})JSON";

// JavaScript（k6 脚本）：关键字/字符串（含模板串）/注释/数字/k6 常用全局
// （http/check/sleep/group 等染 builtin）。函数调用染 method。
inline constexpr std::string_view kJavaScriptSyntax = R"JSON({
  "name": "javascript",
  "fileSuffixes": [".js", ".mjs"],
  "variables": {"identifier": "[a-zA-Z_$][a-zA-Z0-9_$]*"},
  "fragments": {
    "urlRule": [
      {"pattern": "https?://[^\\s\"'`]+", "style": "url"}
    ],
    "templateStringRule": [
      {"pattern": "(`)((?:[^`\\\\]|\\\\.)*)(`)", "styles": [1, "string", 2, "string", 3, "string"]}
    ],
    "doubleQuotedStringRule": [
      {"pattern": "(\")((?:[^\"\\\\]|\\\\.)*)(\")", "styles": [1, "string", 2, "string", 3, "string"]}
    ],
    "singleQuotedStringRule": [
      {"pattern": "(')((?:[^'\\\\]|\\\\.)*)(')", "styles": [1, "string", 2, "string", 3, "string"]}
    ],
    "lineCommentRule": [
      {"pattern": "(//)(.*)", "styles": [1, "comment", 2, "comment"]}
    ],
    "blockCommentRule": [
      {"pattern": "(/\\*)([\\S\\s]*?)(\\*/)", "styles": [1, "comment", 2, "comment", 3, "comment"]}
    ]
  },
  "states": {
    "default": [
      {"include": "blockCommentRule"},
      {"include": "lineCommentRule"},
      {"include": "templateStringRule"},
      {"include": "doubleQuotedStringRule"},
      {"include": "singleQuotedStringRule"},
      {"include": "urlRule"},
      {"pattern": "\\b(?:import|from|export|default|const|let|var|function|return|if|else|for|while|do|break|continue|new|typeof|instanceof|in|of|try|catch|finally|throw|async|await|yield|class|extends|switch|case|this|null|undefined|true|false)\\b", "style": "keyword"},
      {"pattern": "\\b(?:http|check|checkStatus|sleep|group|fail|expect|JSON|Math|Date|console)\\b", "style": "builtin"},
      {"pattern": "\\b\\d+(?:\\.\\d+)?\\b", "style": "number"},
      {"pattern": "(${identifier})(\\()", "styles": [1, "method", 2, "punctuation"]},
      {"pattern": "[{}()\\[\\];,.:<>=+*/%!&|?-]", "style": "punctuation"}
    ]
  },
  "scopeRules": {
    "skips": [
      {"kind": "blockComment", "start": "/*", "end": "*/"},
      {"kind": "lineComment", "start": "//"},
      {"kind": "string", "start": "\"", "end": "\"", "escape": "\\"},
      {"kind": "string", "start": "'", "end": "'", "escape": "\\"},
      {"kind": "string", "start": "`", "end": "`", "escape": "\\"}
    ]
  },
  "bracketRules": {"pairs": [{"start": "(", "end": ")"}, {"start": "[", "end": "]"}, {"start": "{", "end": "}"}]}
})JSON";

// 纯文本（Text 等无结构 body）：只染 URL，其余不着色。
inline constexpr std::string_view kPlainSyntax = R"JSON({
  "name": "plain",
  "fileSuffixes": [".txt"],
  "states": {
    "default": [
      {"pattern": "https?://[^\\s]+", "style": "url"}
    ]
  }
})JSON";

// XML：注释/CDATA/标签名（keyword）/属性名（variable）/字符串/实体引用（macro）。
inline constexpr std::string_view kXmlSyntax = R"JSON({
  "name": "xml",
  "fileSuffixes": [".xml", ".html", ".svg"],
  "fragments": {
    "commentRule": [
      {"pattern": "(<!--)([\\S\\s]*?)(-->)", "styles": [1, "comment", 2, "comment", 3, "comment"]}
    ],
    "cdataRule": [
      {"pattern": "(<!\\[CDATA\\[)([\\S\\s]*?)(\\]\\]>)", "styles": [1, "punctuation", 2, "string", 3, "punctuation"]}
    ],
    "stringRule": [
      {"pattern": "(\")((?:[^\"\\\\]|\\\\.)*)(\")", "styles": [1, "string", 2, "string", 3, "string"]}
    ]
  },
  "states": {
    "default": [
      {"include": "commentRule"},
      {"include": "cdataRule"},
      {"include": "stringRule"},
      {"pattern": "<\\?", "style": "punctuation"},
      {"pattern": "(</?)([a-zA-Z_][\\w:.-]*)", "styles": [1, "punctuation", 2, "keyword"]},
      {"pattern": "([a-zA-Z_][\\w:.-]*)(=)", "styles": [1, "variable", 2, "punctuation"]},
      {"pattern": "/?\\?*>", "style": "punctuation"},
      {"pattern": "&(?:#\\d+|#x[0-9a-fA-F]+|[a-zA-Z][\\w]*);", "style": "macro"}
    ]
  },
  "scopeRules": {
    "skips": [
      {"kind": "blockComment", "start": "<!--", "end": "-->"},
      {"kind": "string", "start": "\"", "end": "\"", "escape": "\\"}
    ]
  },
  "bracketRules": {"pairs": [{"start": "<", "end": ">"}]}
})JSON";

// GraphQL：# 注释/字符串（含 """ 块串）/操作与类型关键字/大写类型名（class）/
// $变量（variable）/数字/标点。
inline constexpr std::string_view kGraphqlSyntax = R"JSON({
  "name": "graphql",
  "fileSuffixes": [".graphql", ".gql"],
  "fragments": {
    "lineCommentRule": [
      {"pattern": "(#)(.*)", "styles": [1, "comment", 2, "comment"]}
    ],
    "blockStringRule": [
      {"pattern": "(\"\"\")([\\S\\s]*?)(\"\"\")", "styles": [1, "string", 2, "string", 3, "string"]}
    ],
    "stringRule": [
      {"pattern": "(\")((?:[^\"\\\\]|\\\\.)*)(\")", "styles": [1, "string", 2, "string", 3, "string"]}
    ]
  },
  "states": {
    "default": [
      {"include": "lineCommentRule"},
      {"include": "blockStringRule"},
      {"include": "stringRule"},
      {"pattern": "\\b(?:query|mutation|subscription|fragment|on|schema|type|interface|union|enum|input|extend|scalar|directive|implements)\\b", "style": "keyword"},
      {"pattern": "\\b(?:true|false|null)\\b", "style": "keyword"},
      {"pattern": "\\$[a-zA-Z_][\\w]*", "style": "variable"},
      {"pattern": "\\b[A-Z][A-Za-z0-9_]*\\b", "style": "class"},
      {"pattern": "-?\\d+(?:\\.\\d+)?(?:[eE][+-]?\\d+)?", "style": "number"},
      {"pattern": "\\.\\.\\.|[{}()\\[\\]:=@!,]", "style": "punctuation"}
    ]
  },
  "scopeRules": {
    "skips": [
      {"kind": "lineComment", "start": "#"},
      {"kind": "string", "start": "\"", "end": "\"", "escape": "\\"}
    ]
  },
  "bracketRules": {"pairs": [{"start": "{", "end": "}"}, {"start": "(", "end": ")"}, {"start": "[", "end": "]"}]}
})JSON";

} // namespace apitab::ui
