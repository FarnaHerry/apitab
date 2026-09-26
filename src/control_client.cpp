// control_client.cpp — `apitab --cli …` 的客户端半边（见 control.h）。
//
// 这个进程**不构造 store、不碰数据库、不进事件循环**：它只读端点文件、把 argv 发给
// 运行中的实例、把回传的 stdout/stderr 原样回放并沿用它的退出码。命令实现在实例里
// （src/cli.cpp 同一份），所以输出契约与 "--cli 在实例内直跑" 完全一致。
#include <huxerui/huxerui.h>

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "control.h"

import asio;
import nlohmann.json;

namespace apitab::control {
namespace {

using json = nlohmann::json;

// 客户端侧不需要运行时，但保留与仓库其它 TU 一致的 asio 引入方式（import asio）。
constexpr int kConnectTimeoutMs = 4000;

} // namespace

int ForwardCommand(const std::vector<std::string>& args, std::string& error) {
    int port = 0;
    std::string token;
    if (!ReadEndpoint(port, token, error)) return 1;

    asio::io_context context;
    asio::ip::tcp::socket socket{context};
    asio::error_code ec;
    const asio::ip::tcp::endpoint endpoint{
        asio::ip::make_address("127.0.0.1", ec), static_cast<unsigned short>(port)};
    if (ec) {
        error = std::string{"控制面端点无效: "} + ec.message();
        return 1;
    }
    socket.connect(endpoint, ec);
    if (ec) {
        error = std::string{"连接运行中的 apitab 失败（127.0.0.1:"} + std::to_string(port) +
                "）: " + ec.message();
        return 1;
    }

    const std::string payload = json{{"args", args}}.dump();
    std::string request = "POST /v1/command HTTP/1.1\r\n";
    request += "Host: 127.0.0.1\r\n";
    request += "Authorization: Bearer " + token + "\r\n";
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += payload;
    asio::write(socket, asio::buffer(request), ec);
    if (ec) {
        error = std::string{"发送命令失败: "} + ec.message();
        return 1;
    }

    // 服务端 Connection: close，所以读到 EOF 即整条响应。手工缓冲读取：
    // 本构建定义 ASIO_NO_IOSTREAM，asio::streambuf/read_until 不可用。
    std::string response_text;
    char chunk[4096];
    for (;;) {
        const std::size_t n = socket.read_some(asio::buffer(chunk), ec);
        if (ec == asio::error::eof) break;
        if (ec) {
            error = std::string{"读取响应失败: "} + ec.message();
            return 1;
        }
        response_text.append(chunk, n);
    }

    const std::size_t header_end = response_text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        error = "控制面响应不完整（缺少头块结束）";
        return 1;
    }
    const std::string headers = response_text.substr(0, header_end);
    const auto first_line_end = headers.find("\r\n");
    const std::string status_line = headers.substr(0, first_line_end);
    const auto status_sep = status_line.find(' ');
    const int status = status_sep == std::string::npos
                           ? 0
                           : std::atoi(status_line.substr(status_sep + 1).c_str());
    const std::string response_body = response_text.substr(header_end + 4);

    json response;
    try {
        response = json::parse(response_body);
    } catch (const std::exception& parse_error) {
        error = std::string{"控制面响应无法解析（HTTP "} + std::to_string(status) +
                "）: " + parse_error.what();
        return 1;
    }

    // stdout 只放数据、stderr 放提示——与本地直跑时的分流完全一致。
    std::cout << response.value("stdout", std::string{});
    std::cout.flush();
    std::cerr << response.value("stderr", std::string{});
    std::cerr.flush();
    return response.value("exit", 1);
}

} // namespace apitab::control
