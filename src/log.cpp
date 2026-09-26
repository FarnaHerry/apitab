// log.cpp — 文件日志实现（契约见 log.h）。
//
// 形态：<日志目录>/apitab-YYYYMMDD.log，追加写、每行 flush（审计行不能丢在缓冲里），
// 按天换文件；单文件超过 kMaxBytes 后停止写入并留一行说明，避免失控增长。
// 全程不抛异常——日志失败只降级到 stderr。
#include <huxerui/huxerui.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "log.h"

import apitab.config;

namespace apitab::log {
namespace {

constexpr std::uintmax_t kMaxBytes = 8u * 1024u * 1024u;  // 单日单文件上限（8 MiB）

std::mutex g_mutex;
std::string g_directory;
std::string g_path;
std::ofstream g_file;
std::string g_day;          // 当前文件对应的 YYYYMMDD
bool g_capped = false;      // 当日文件已达上限
bool g_directory_ready = false;

std::string Today() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02d", local.tm_year + 1900,
                  local.tm_mon + 1, local.tm_mday);
    return buffer;
}

std::string Timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                  local.tm_min, local.tm_sec, static_cast<int>(millis));
    return buffer;
}

// 调用方持锁。按天开/换文件；失败返回 false（调用方降级 stderr）。
bool OpenForDay(const std::string& day) {
    if (!g_directory_ready) {
        std::error_code ec;
        std::filesystem::create_directories(g_directory, ec);
        g_directory_ready = !ec;
    }
    const std::filesystem::path path = std::filesystem::path(g_directory) / ("apitab-" + day + ".log");
    g_file.close();
    g_file.open(path, std::ios::binary | std::ios::app);
#ifndef _WIN32
    // 审计行含命令参数（可能带 URL/查询串）：只给本用户读。
    ::chmod(path.c_str(), 0600);
#endif
    g_path = path.string();
    g_day = day;
    g_capped = false;
    return static_cast<bool>(g_file);
}

} // namespace

void Initialize(const std::string& directory) {
    std::lock_guard lock{g_mutex};
    if (!g_directory.empty()) return;  // 只生效第一次
    g_directory = directory.empty() ? (cfg::dataDir() / "logs").string() : directory;
}

std::string CurrentLogPath() {
    std::lock_guard lock{g_mutex};
    return g_path;
}

void Write(std::string_view level, std::string_view message) {
    // 组装整行后再落盘：并发下不会交错，也方便一次 write。
    std::string line;
    try {
        line.reserve(message.size() + 48);
        line += Timestamp();
        line += " [";
        line += level;
        line += "] ";
        line += message;
        line += '\n';
    } catch (...) {
        return;  // 连拼行都失败（OOM）：静默放弃，绝不抛
    }

    std::lock_guard lock{g_mutex};
    try {
        if (g_directory.empty()) g_directory = (cfg::dataDir() / "logs").string();
        const std::string day = Today();
        if (!g_file.is_open() || day != g_day) {
            if (!OpenForDay(day)) {
                std::fwrite(line.data(), 1, line.size(), stderr);
                return;
            }
        }
        if (g_capped) return;
        g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
        g_file.flush();
        if (!g_file) {  // 写失败（磁盘满/权限）：降级 stderr 一次，之后重试仍走这里
            std::fwrite(line.data(), 1, line.size(), stderr);
            g_file.clear();
            return;
        }
        std::error_code ec;
        if (std::filesystem::file_size(g_path, ec) > kMaxBytes && !ec) {
            g_capped = true;
            const std::string capped = Timestamp() +
                " [WARN] 日志超过 8 MiB 上限，本日不再写入（" + g_path + "）\n";
            g_file.write(capped.data(), static_cast<std::streamsize>(capped.size()));
            g_file.flush();
        }
    } catch (...) {
        // 日志路径上的任何异常都不得外泄。
    }
}

void Info(std::string_view message) { Write("INFO", message); }
void Warn(std::string_view message) { Write("WARN", message); }
void Error(std::string_view message) { Write("ERROR", message); }

} // namespace apitab::log
