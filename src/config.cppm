// config.cppm — apitab 配置：用户数据目录（SQLite 落盘位置）与 k6 引擎二进制解析。
// 无 eui 依赖，引擎 / store / UI 共用。
//
// k6 不在 mcpp 包仓库：CI 打包时下载对应平台二进制放进包内 engines/，运行时按
//   1. <exe 目录>/engines/k6(.exe)   —— 打包分发形态
//   2. <exe 目录>/k6(.exe)
//   3. PATH 里的 k6                  —— 开发机已装（本机 /usr/bin/k6）
// 顺序解析。
module;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>  // access, X_OK（executableExists；__APPLE__ 也走 access 分支）
#else
#include <unistd.h>  // readlink, access, X_OK
#include <limits.h>  // PATH_MAX
#endif
#include <cstdio>  // popen / pclose（跟随系统的深色检测）

export module apitab.config;

import std;

namespace cfg {

// 可执行文件目录（k6 相对解析 / 资源回退用）。
export std::filesystem::path executableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf).parent_path();
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) return {};
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::weakly_canonical(buffer).parent_path();
#else
    char buf[PATH_MAX]{};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::filesystem::path(std::string(buf, static_cast<size_t>(n))).parent_path();
#endif
}

// 用户数据目录：Linux $XDG_DATA_HOME/apitab（~/.local/share/apitab）/
// Windows %APPDATA%\apitab / macOS ~/Library/Application Support/apitab。
// SQLite 库文件放这里 —— 安装版启动时 cwd 可能不可写，不能依赖 cwd。
export std::filesystem::path dataDir() {
#ifdef _WIN32
    if (const char* a = std::getenv("APPDATA"); a && *a) {
        return std::filesystem::path(a) / "apitab";
    }
#elif defined(__APPLE__)
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / "Library" / "Application Support" / "apitab";
    }
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) {
        return std::filesystem::path(x) / "apitab";
    }
    if (const char* h = std::getenv("HOME"); h && *h) {
        return std::filesystem::path(h) / ".local" / "share" / "apitab";
    }
#endif
    return std::filesystem::current_path();
}

// SQLite 数据库文件路径（确保父目录存在）。
export std::filesystem::path databaseFile() {
    const std::filesystem::path dir = dataDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "apitab.db";
}

bool executableExists(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec) || ec) return false;
#ifdef _WIN32
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

// PATH 查找（which 语义）。
std::filesystem::path findInPath(std::string_view name) {
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return {};
#ifdef _WIN32
    constexpr char sep = ';';
#else
    constexpr char sep = ':';
#endif
    std::string_view rest{pathEnv};
    while (!rest.empty()) {
        const auto pos = rest.find(sep);
        const std::string_view dir = rest.substr(0, pos);
        rest = (pos == std::string_view::npos) ? std::string_view{} : rest.substr(pos + 1);
        if (dir.empty()) continue;
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (executableExists(candidate)) return candidate;
    }
    return {};
}

// k6 二进制解析：exe 旁 engines/ → exe 旁 → PATH。找不到返回空路径。
export std::filesystem::path k6Binary() {
#ifdef _WIN32
    constexpr std::string_view k6Name = "k6.exe";
#else
    constexpr std::string_view k6Name = "k6";
#endif
    const std::filesystem::path exeDir = executableDir();
    if (!exeDir.empty()) {
        if (const auto p = exeDir / "engines" / k6Name; executableExists(p)) return p;
        if (const auto p = exeDir / k6Name; executableExists(p)) return p;
    }
    // 开发形态：mcpp 的 exe 在 target/<triple>/<hash>/bin/，仓库根的 engines/ 是其
    // 四层之上的 engines/ —— 开发时把 k6 放 <repo>/engines/k6 也能找到（CI 打包同理）。
    if (!exeDir.empty()) {
        if (const auto p = exeDir / ".." / ".." / ".." / ".." / "engines" / k6Name;
            executableExists(p)) {
            return std::filesystem::weakly_canonical(p);
        }
    }
    return findInPath(k6Name);
}

// ---- 系统查询用到的 C 资源（RAII）-------------------------------------------
// popen 的 FILE* 与 Win32 注册表键都必须由类型持有：函数体内的查询语句本身虽
// 不抛，但"创建 → 使用 → 释放"之间任何后续维护（给 fgets 换更长的读取、加日志、
// 提前返回）都会让手写 cleanup 漏掉。约定见 CLAUDE.md「关键约定」第 9 条。
#if !defined(_WIN32)
class PopenPipe {
public:
    explicit PopenPipe(const char* command) : file_(::popen(command, "r")) {}
    ~PopenPipe() {
        if (file_ != nullptr) ::pclose(file_);
    }

    PopenPipe(const PopenPipe&) = delete;
    PopenPipe& operator=(const PopenPipe&) = delete;

    bool valid() const { return file_ != nullptr; }

    // 读一行到 buf；返回是否读到内容（同 std::fgets 语义）。
    bool readLine(std::span<char> buf) const {
        return file_ != nullptr &&
               std::fgets(buf.data(), static_cast<int>(buf.size()), file_) != nullptr;
    }

private:
    FILE* file_ = nullptr;
};
#else
// Win32 注册表键：析构 RegCloseKey；打开失败时空操作。
class RegKey {
public:
    RegKey(HKEY root, const char* subkey) {
        opened_ = ::RegOpenKeyExA(root, subkey, 0, KEY_READ, &key_) == ERROR_SUCCESS;
    }
    ~RegKey() {
        if (opened_) ::RegCloseKey(key_);
    }

    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;

    bool opened() const { return opened_; }

    LONG queryDword(const char* name, DWORD& value) const {
        DWORD size = sizeof(value);
        return ::RegQueryValueExA(key_, name, nullptr, nullptr,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    }

private:
    HKEY key_ = nullptr;
    bool opened_ = false;
};
#endif

// 系统是否偏好深色（"跟随系统"主题模式用）。启动时读取一次即可。
export bool systemPrefersDark() {
#if defined(_WIN32)
    const RegKey key{HKEY_CURRENT_USER,
                     "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"};
    if (!key.opened()) return false;
    DWORD value = 1;
    return key.queryDword("AppsUseLightTheme", value) == ERROR_SUCCESS && value == 0;
#elif defined(__APPLE__)
    const PopenPipe pipe{"defaults read -g AppleInterfaceStyle 2>/dev/null"};
    if (!pipe.valid()) return false;
    std::array<char, 32> buf{};
    return pipe.readLine(buf) && std::string_view(buf.data()).starts_with("Dark");
#else
    const PopenPipe pipe{"gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null"};
    if (!pipe.valid()) return false;
    std::array<char, 64> buf{};
    return pipe.readLine(buf) &&
           std::string_view(buf.data()).find("prefer-dark") != std::string_view::npos;
#endif
}

} // namespace cfg
