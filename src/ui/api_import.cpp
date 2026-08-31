// api_import.cpp — 接口文件解析实现（契约见 api_import.h；纯文本进、结构出，不依赖 UI）。
//
// 支持矩阵（三种格式自动识别；只吃 JSON；异常不外抛）：
//   • Swagger 2.0：paths × {get,put,post,delete,options,head,patch,trace}；
//     parameters path 级 + operation 级合并（operation 覆盖同名 in+name）；
//     in=query→params、in=header→headers、其余 in 跳过；in=body→"json" 体、
//     in=formData→"form" 体（k=v& 拼接）；tags[0]→目录链；host 特例：
//     host 非空且不含 '{' 时 schemes[0](默认 https)+host+basePath+path 拼 fullUrl。
//   • OpenAPI 3.x：同上方法表；参数值 = schema.example || schema.default ||
//     example；requestBody content 优先 application/json→"json"、
//     *x-www-form-urlencoded/multipart/form-data→"form"、*xml→"xml"、
//     其余首个→"text"；body = mediaType.example || examples 首项 .example
//     （或 .value）|| schema.example || schema.default（object/array 单行 dump，
//     标量转字符串，没有则空）。
//   • Postman Collection v2.x：递归 item[]，folder 名进目录链；url string 直接
//     用（{{var}} 原样保留，工具环境替换处理），object 优先 raw、否则
//     host+path 数组按 '/' 拼；http(s):// 前缀→fullUrl；query/header→
//     params/headers（disabled 行跳过；value null=空串）；body mode=raw
//     （options.raw.language=="json"→"json" 否则 "text"）、urlencoded/formdata
//     →"form"（各 field 以 k=v&k2=v2 拼接；原 content-type 头作为普通 header 保留）、
//     graphql→"graphql"（body=query 文本）。
//
// 已知不支持：YAML（报可读错误，请先导出为 JSON）；auth；$ref 引用的参数/组件
// 定义不展开（$ref 参数与 $ref operation 直接跳过）；responses 不导；
// servers/baseUrl 不导（由工具环境 baseUrl 负责拼接）。
#include "api_import.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <typeinfo>  // GCC -fmodules-ts：import nlohmann.json 的延迟模板实例化需要 typeid
#include <vector>

import nlohmann.json;

namespace apitab::ui {
namespace {

using json = nlohmann::json;

std::string trim(const std::string& s) {
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toUpper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 取对象字符串成员；非 object / 缺失 / 非字符串一律空串（全 is_xxx 防御，不抛）。
std::string strOr(const json& obj, const char* key) {
    if (obj.is_object() && obj.contains(key) && obj[key].is_string()) return obj[key].get<std::string>();
    return {};
}

// JSON 值 → 文本：字符串原样；null=空；数字/布尔/object/array = 单行紧凑 dump()。
std::string jsonToText(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return {};
    return v.dump();
}

// 读非 null 成员转文本；命中返回 true。
bool textMember(const json& obj, const char* key, std::string& out) {
    if (obj.is_object() && obj.contains(key) && !obj[key].is_null()) {
        out = jsonToText(obj[key]);
        return true;
    }
    return false;
}

std::string joinAmp(const std::vector<std::string>& parts) {
    std::string s;
    for (const auto& p : parts) {
        if (!s.empty()) s += '&';
        s += p;
    }
    return s;
}

// ---- OpenAPI 3.x / Swagger 2.0 -----------------------------------------

// 并入 parameters[]：跳过非 object、$ref、缺 in|name 的条目；同名（in+name）覆盖。
void mergeParams(const json& arr, std::vector<json>& merged) {
    if (!arr.is_array()) return;
    for (const json& p : arr) {
        if (!p.is_object() || p.contains("$ref")) continue;
        const std::string in = strOr(p, "in");
        const std::string name = strOr(p, "name");
        if (in.empty() || name.empty()) continue;
        bool replaced = false;
        for (json& m : merged) {
            if (strOr(m, "in") == in && strOr(m, "name") == name) {
                m = p;
                replaced = true;
                break;
            }
        }
        if (!replaced) merged.push_back(p);
    }
}

// 参数值：3.x = schema.example || schema.default || example；2.0 = example || default。
std::string paramValue(const json& p, bool swagger2) {
    std::string v;
    if (!swagger2 && p.contains("schema") && p["schema"].is_object()) {
        if (textMember(p["schema"], "example", v) || textMember(p["schema"], "default", v)) return v;
    }
    if (textMember(p, "example", v) || textMember(p, "default", v)) return v;
    return {};
}

// 示例体取值链：example → examples 首项的 example/value → schema.example → schema.default。
// 同时用于 3.x 媒体类型对象与 2.0 in=body 参数（后者只有 example + schema 分支会命中）。
std::string mediaBody(const json& mt) {
    std::string v;
    if (textMember(mt, "example", v)) return v;
    if (mt.contains("examples") && mt["examples"].is_object()) {
        for (auto it = mt["examples"].begin(); it != mt["examples"].end(); ++it) {
            if (!it.value().is_object()) continue;
            if (textMember(it.value(), "example", v) || textMember(it.value(), "value", v)) return v;
        }
    }
    if (mt.contains("schema") && mt["schema"].is_object()) {
        if (textMember(mt["schema"], "example", v) || textMember(mt["schema"], "default", v)) return v;
    }
    return {};
}

std::string normalizeMediaType(std::string mt) {
    const auto semi = mt.find(';');
    if (semi != std::string::npos) mt = mt.substr(0, semi);
    return toLower(trim(mt));
}

// 3.x requestBody → bodyKind/body；无 content 或畸形则不动。
void applyRequestBody3(const json& op, ImportedOperation& io) {
    if (!op.contains("requestBody") || !op["requestBody"].is_object()) return;
    const json& rb = op["requestBody"];
    if (!rb.contains("content") || !rb["content"].is_object()) return;
    const json& content = rb["content"];
    std::vector<std::pair<std::string, const json*>> entries;
    for (auto it = content.begin(); it != content.end(); ++it) {
        if (it.value().is_object()) entries.emplace_back(normalizeMediaType(it.key()), &it.value());
    }
    const auto isJson = [](const std::string& m) { return m == "application/json"; };
    const auto isForm = [](const std::string& m) {
        return m.find("x-www-form-urlencoded") != std::string::npos
            || m.find("multipart/form-data") != std::string::npos;
    };
    const auto isXml = [](const std::string& m) { return m.find("xml") != std::string::npos; };
    const json* pick = nullptr;
    const char* kind = "";
    const auto tryPick = [&](const auto& pred, const char* name) {
        for (const auto& e : entries) {
            if (pred(e.first)) { pick = e.second; kind = name; return true; }
        }
        return false;
    };
    if (!tryPick(isJson, "json") && !tryPick(isForm, "form") && !tryPick(isXml, "xml")
        && !entries.empty()) {
        pick = entries.front().second;
        kind = "text";
    }
    if (pick) {
        io.bodyKind = kind;
        io.body = mediaBody(*pick);
    }
}

void parseOpenApi(const json& root, ImportedApi& out) {
    const bool swagger2 = !root.contains("openapi");  // 族判定：无 openapi 键即 swagger(2.x)
    if (root.contains("info") && root["info"].is_object()) out.title = strOr(root["info"], "title");

    // Swagger2 host 特例：host 非空且不含 '{'（模板）→ 拼完整 URL、fullUrl=true。
    std::string prefix;
    bool fullUrl = false;
    if (swagger2) {
        const std::string host = strOr(root, "host");
        if (!host.empty() && host.find('{') == std::string::npos) {
            std::string scheme = "https";
            if (root.contains("schemes") && root["schemes"].is_array() && !root["schemes"].empty()
                && root["schemes"][0].is_string())
                scheme = root["schemes"][0].get<std::string>();
            prefix = scheme + "://" + host + strOr(root, "basePath");
            fullUrl = true;
        }
    }

    if (!root.contains("paths") || !root["paths"].is_object()) return;
    static constexpr const char* kMethods[] = {"get", "put", "post", "delete",
                                               "options", "head", "patch", "trace"};
    for (auto pit = root["paths"].begin(); pit != root["paths"].end(); ++pit) {
        if (!pit.value().is_object()) continue;
        const std::string path = pit.key();  // 路径模板：{param} 段原样保留
        const json& pathItem = pit.value();
        std::vector<json> shared;
        if (pathItem.contains("parameters")) mergeParams(pathItem["parameters"], shared);
        for (const char* m : kMethods) {
            if (!pathItem.contains(m) || !pathItem[m].is_object()) continue;  // operation 非 object 防御
            const json& op = pathItem[m];
            if (op.contains("$ref")) continue;  // operation 级 $ref 不展开
            ImportedOperation io;
            io.method = toUpper(m);
            io.url = prefix + path;
            io.fullUrl = fullUrl;
            const std::string summary = strOr(op, "summary");
            const std::string opId = strOr(op, "operationId");
            io.name = !summary.empty() ? summary : (!opId.empty() ? opId : io.method + " " + path);
            if (op.contains("tags") && op["tags"].is_array() && !op["tags"].empty()
                && op["tags"][0].is_string()) {
                const std::string tag = op["tags"][0].get<std::string>();
                if (!tag.empty()) io.dirChain.push_back(tag);  // 仅第一个 tag
            }
            std::vector<json> params = shared;
            if (op.contains("parameters")) mergeParams(op["parameters"], params);
            std::vector<std::string> formFields;
            for (const json& p : params) {
                const std::string in = strOr(p, "in");
                const bool swaggerBodyIn = swagger2 && (in == "body" || in == "formData");
                if (in != "query" && in != "header" && !swaggerBodyIn) continue;  // path/cookie 等跳过
                ImportedParam ip;
                ip.key = strOr(p, "name");
                ip.value = paramValue(p, swagger2);
                ip.remark = strOr(p, "description");
                if (in == "query") {
                    io.params.push_back(ip);
                } else if (in == "header") {
                    io.headers.push_back(ip);
                } else if (in == "body") {
                    if (io.bodyKind.empty()) {
                        io.bodyKind = "json";
                        io.body = mediaBody(p);
                    }
                } else {  // formData：集合类表单参数 → form 体
                    formFields.push_back(ip.key + "=" + ip.value);
                }
            }
            if (!swagger2) applyRequestBody3(op, io);
            if (!formFields.empty() && io.bodyKind.empty()) {
                io.bodyKind = "form";
                io.body = joinAmp(formFields);
            }
            out.operations.push_back(std::move(io));
        }
    }
}

// ---- Postman Collection v2.x --------------------------------------------

bool startsWithHttp(const std::string& s) {
    const std::string head = toLower(s.substr(0, 8));
    return head.rfind("http://", 0) == 0 || head.rfind("https://", 0) == 0;
}

// query[]/header[] 条目 → ImportedParam：非 object/disabled/无 key 跳过；value 可为 null（空串）；
// description 支持 string 或 {content} 两种写法。
void postmanKvs(const json& arr, std::vector<ImportedParam>& dst) {
    if (!arr.is_array()) return;
    for (const json& e : arr) {
        if (!e.is_object()) continue;
        if (e.contains("disabled") && e["disabled"].is_boolean() && e["disabled"].get<bool>()) continue;
        ImportedParam ip;
        ip.key = strOr(e, "key");
        if (ip.key.empty()) continue;
        if (e.contains("value") && !e["value"].is_null()) ip.value = jsonToText(e["value"]);
        if (e.contains("description")) {
            if (e["description"].is_string()) ip.remark = e["description"].get<std::string>();
            else if (e["description"].is_object()) ip.remark = strOr(e["description"], "content");
        }
        dst.push_back(std::move(ip));
    }
}

// url 成员：string 直接用；object 优先 raw，否则 host+path 数组按 '/' 拼。
// object 形态顺带带出 query[]。
std::string postmanUrl(const json& u, const json*& queryArr) {
    if (u.is_string()) return trim(u.get<std::string>());
    if (!u.is_object()) return {};
    if (u.contains("query") && u["query"].is_array()) queryArr = &u["query"];
    std::string raw = trim(strOr(u, "raw"));
    if (!raw.empty()) return raw;
    std::string s;
    const auto append = [&](const char* key) {
        if (!u.contains(key) || !u[key].is_array()) return;
        for (const json& part : u[key]) {
            if (!part.is_string() || part.get<std::string>().empty()) continue;
            if (!s.empty()) s += '/';
            s += part.get<std::string>();
        }
    };
    append("host");
    append("path");
    return s;
}

void parsePostmanBody(const json& req, ImportedOperation& io) {
    if (!req.contains("body") || !req["body"].is_object()) return;
    const json& body = req["body"];
    const std::string mode = strOr(body, "mode");
    if (mode == "raw") {
        io.bodyKind = "text";
        if (req.contains("options") && req["options"].is_object()
            && req["options"].contains("raw") && req["options"]["raw"].is_object()
            && strOr(req["options"]["raw"], "language") == "json")
            io.bodyKind = "json";
        io.body = strOr(body, "raw");
    } else if (mode == "urlencoded" || mode == "formdata") {
        io.bodyKind = "form";
        std::vector<std::string> fields;
        const char* arrKey = mode == "urlencoded" ? "urlencoded" : "formdata";
        if (body.contains(arrKey) && body[arrKey].is_array()) {
            for (const json& f : body[arrKey]) {
                if (!f.is_object()) continue;
                if (f.contains("disabled") && f["disabled"].is_boolean() && f["disabled"].get<bool>()) continue;
                const std::string k = strOr(f, "key");
                if (k.empty()) continue;
                std::string v;
                if (f.contains("value") && !f["value"].is_null()) v = jsonToText(f["value"]);
                fields.push_back(k + "=" + v);
            }
        }
        io.body = joinAmp(fields);
        // 注意：原 header[] 里的 Content-Type 已由 postmanKvs 原样保留，这里不再增删。
    } else if (mode == "graphql") {
        io.bodyKind = "graphql";
        if (body.contains("graphql") && body["graphql"].is_object())
            io.body = strOr(body["graphql"], "query");
    }
    // 其余 mode（file 等）：body 不导。
}

void parsePostmanItems(const json& items, const std::vector<std::string>& chain,
                       ImportedApi& out, int depth) {
    if (!items.is_array() || depth > 64) return;  // 深度上限：防畸形嵌套自递归
    for (const json& it : items) {
        if (!it.is_object()) continue;
        const std::string name = strOr(it, "name");
        if (it.contains("item") && it["item"].is_array()) {  // folder：名字进目录链，递归
            std::vector<std::string> sub = chain;
            if (!name.empty()) sub.push_back(name);
            parsePostmanItems(it["item"], sub, out, depth + 1);
            continue;
        }
        if (!it.contains("request")) continue;
        const json& req = it["request"];
        ImportedOperation io;
        io.dirChain = chain;
        io.name = name;
        const json* queryArr = nullptr;
        if (req.is_string()) {  // 简写形态："request": "https://…"
            io.method = "GET";
            io.url = trim(req.get<std::string>());
        } else if (req.is_object()) {
            io.method = toUpper(strOr(req, "method"));
            if (io.method.empty()) continue;  // 缺 method 视为畸形条目
            if (req.contains("url")) io.url = postmanUrl(req["url"], queryArr);
            io.fullUrl = startsWithHttp(io.url);
            if (req.contains("header")) postmanKvs(req["header"], io.headers);
            parsePostmanBody(req, io);
        } else {
            continue;
        }
        if (queryArr) postmanKvs(*queryArr, io.params);
        out.operations.push_back(std::move(io));
    }
}

void parsePostman(const json& root, ImportedApi& out) {
    // 集合名在 info.name（v2 规范）；兼容个别把 name 放顶层的导出。
    out.title = strOr(root["info"], "name");
    if (out.title.empty()) out.title = strOr(root, "name");
    parsePostmanItems(root["item"], {}, out, 0);
}

} // namespace

bool ParseApiFile(const std::string& text, ImportedApi& out, std::string& error) {
    out = ImportedApi{};
    error.clear();
    try {
        std::string body = text;
        if (body.starts_with("\xEF\xBB\xBF")) body.erase(0, 3);  // UTF-8 BOM
        body = trim(body);
        if (body.empty()) {
            error = "文件内容为空";
            return false;
        }
        if (body.front() != '{') {
            error = "暂仅支持 JSON 格式（YAML 请先导出为 JSON）";
            return false;
        }
        const json root = json::parse(body, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            error = "JSON 解析失败";
            return false;
        }
        if (root.contains("swagger") || root.contains("openapi")) {
            parseOpenApi(root, out);
        } else if (root.contains("info") && root["info"].is_object()
                   && root.contains("item") && root["item"].is_array()) {
            if (strOr(root["info"], "schema").find("collection") == std::string::npos) {
                error = "无法识别格式（支持 OpenAPI/Swagger/Postman 集合）";
                return false;
            }
            parsePostman(root, out);
        } else {
            error = "无法识别格式（支持 OpenAPI/Swagger/Postman 集合）";
            return false;
        }
        if (out.operations.empty()) {
            error = "未解析到任何接口操作";
            return false;
        }
        return true;
    } catch (...) {
        // 理论上所有路径都有 is_xxx/contains 防御；此处兜底保证绝不向外抛。
        error = "解析异常：文件结构异常";
        return false;
    }
}

} // namespace apitab::ui
