// api_import.h — 接口文件导入的解析层（UI 无关，纯文本进、结构出）。
// 实现见 api_import.cpp。支持：Swagger 2.0 / OpenAPI 3.x（JSON）、Postman
// Collection v2.x（JSON）。格式自动识别；YAML 暂不支持（报可读错误）。
#pragma once

#include <string>
#include <vector>

namespace apitab::ui {

// 导入的键值条目（参数/头）。value = example/default（可为空），remark = 描述。
struct ImportedParam {
    std::string key;
    std::string value;
    std::string remark;
};

// 一条导入的接口操作。
struct ImportedOperation {
    std::string method;                 // 大写 HTTP 方法名
    std::string url;                    // path（与 baseUrl 拼接）或完整 URL
    bool fullUrl = false;               // true = url 已含 scheme，直接用
    std::string name;                   // summary / operationId / Postman 名
    std::vector<std::string> dirChain;  // 接口目录链（tag / folder），空 = 根
    std::vector<ImportedParam> params;  // query 参数
    std::vector<ImportedParam> headers; // header 参数
    // bodyKind："" = 无；"json"/"text"/"xml"/"form"/"graphql"（其余按 text）。
    std::string bodyKind;
    std::string body;                   // 示例体（schema example 等，可空）
};

struct ImportedApi {
    std::string title;                  // info.title / collection name
    std::vector<ImportedOperation> operations;
};

// 解析文件全文。成功返回 true；失败返回 false 并填 error（中文、可直接 toast）。
bool ParseApiFile(const std::string& text, ImportedApi& out, std::string& error);

} // namespace apitab::ui
