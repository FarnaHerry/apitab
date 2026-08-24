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

// RFC 3986 unreserved 之外全部 %XX（query 参数编码，不依赖 curl —— k6 引擎也用）。
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
