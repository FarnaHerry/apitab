// utils.cppm — 纯 string/number 帮助函数（无 UI / 引擎依赖，store / 引擎 / UI 共用）。
module;

#include <time.h>  // localtime_r / localtime_s（C 函数不在 import std 里）

export module apitab.utils;

import std;

// 去掉首尾空白（URL / header 键值输入清洗）。
export std::string trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::ranges::find_if(s, notSpace));
    s.erase(std::ranges::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// 毫秒数格式化：1234.5 -> "1.23 s"，123.4 -> "123 ms"。
export std::string formatMs(double ms) {
    if (ms >= 1000.0) return std::format("{:.2f} s", ms / 1000.0);
    return std::format("{:.0f} ms", ms);
}

// 字节数格式化：2048 -> "2.0 KB"。
export std::string formatBytes(std::int64_t bytes) {
    if (bytes < 0) return "-";
    constexpr double k = 1024.0;
    if (bytes < k) return std::format("{} B", bytes);
    if (bytes < k * k) return std::format("{:.1f} KB", bytes / k);
    if (bytes < k * k * k) return std::format("{:.1f} MB", bytes / (k * k));
    return std::format("{:.2f} GB", bytes / (k * k * k));
}

// 比率格式化：0.1234 -> "12.34%"。
export std::string formatPct(double ratio) {
    return std::format("{:.2f}%", ratio * 100.0);
}

// Unix 秒 -> "MM-DD HH:MM"（本地时间，历史/压测记录列表用）。
export std::string formatTime(std::int64_t unixSec) {
    const std::time_t t = static_cast<std::time_t>(unixSec);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:02d}-{:02d} {:02d}:{:02d}",
                       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
}

export std::int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 判断开头是否为 RFC 3986 URI scheme（如 https:、custom+api:）。
export bool hasUriScheme(std::string_view value) {
    if (value.empty() || !((value.front() >= 'A' && value.front() <= 'Z') ||
                           (value.front() >= 'a' && value.front() <= 'z'))) {
        return false;
    }
    for (std::size_t i = 1; i < value.size(); ++i) {
        const char c = value[i];
        if (c == ':') return true;
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        if (!valid) return false;
    }
    return false;
}

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decodeQueryComponent(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '%' && i + 2 < value.size()) {
            const int high = hexValue(value[i + 1]);
            const int low = hexValue(value[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        // application/x-www-form-urlencoded（浏览器复制的 URL 最常见的形式）
        // 使用 '+' 表示空格；普通百分号编码仍按 RFC 3986 解码。
        decoded.push_back(c == '+' ? ' ' : c);
    }
    return decoded;
}

} // namespace

// URL 输入中的 query 解析结果。URL 字段只保留 query 前的部分，参数交给
// Params 表维护，避免发送时再同时携带一份 URL query 和一份 Params。
export struct UrlQueryParseResult {
    std::string url;
    std::vector<std::pair<std::string, std::string>> params;
    bool intercepted = false;
};

// 识别并拆分 URL 中的 query。只接收标准的 key=value 项；不完整的项被忽略，
// 但 '?' 仍会被截断，以便用户在 Params 表中补齐。片段（#fragment）不属于
// query，也不应被发送，因此一并忽略。
export UrlQueryParseResult parseUrlQuery(std::string_view input) {
    UrlQueryParseResult result{.url = std::string(input)};
    const std::size_t question = input.find('?');
    if (question == std::string_view::npos) return result;

    result.intercepted = true;
    result.url.assign(input.substr(0, question));
    std::string_view query = input.substr(question + 1);
    if (const std::size_t fragment = query.find('#');
        fragment != std::string_view::npos) {
        query = query.substr(0, fragment);
    }

    std::size_t begin = 0;
    while (begin <= query.size()) {
        const std::size_t end = query.find('&', begin);
        const std::string_view item = query.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        const std::size_t equals = item.find('=');
        if (equals != std::string_view::npos) {
            std::string key = decodeQueryComponent(item.substr(0, equals));
            if (!key.empty()) {
                result.params.emplace_back(std::move(key),
                                           decodeQueryComponent(item.substr(equals + 1)));
            }
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return result;
}

export std::string percentEncode(std::string_view s) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// 把启用的 query 参数拼进 URL（处理已有 ? / & 与空值）。
export std::string appendQuery(std::string url,
                               const std::vector<std::pair<std::string, std::string>>& params) {
    for (const auto& [k, v] : params) {
        if (k.empty()) continue;
        url.push_back(url.find('?') == std::string::npos ? '?' : '&');
        url += percentEncode(k);
        url.push_back('=');
        url += percentEncode(v);
    }
    return url;
}
