// preferences.cppm — 会话偏好（settings.ini 的 session.* 键值）。
// 语言 / 主题等外观偏好由 HuxerUI 主题系统承担；此模块只承载跨会话的应用状态恢复。
export module apitab.preferences;

import std;
import apitab.config;

export std::unordered_map<std::string, std::string> g_sessionPreference;

namespace {

std::filesystem::path settingsFile() { return cfg::dataDir() / "settings.ini"; }

} // namespace

export void loadSessionPreferences() {
    std::ifstream input(settingsFile());
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("session.")) {
            const std::size_t equal = line.find('=');
            if (equal != std::string::npos) {
                g_sessionPreference[line.substr(8, equal - 8)] = line.substr(equal + 1);
            }
        }
    }
}

export void saveSessionPreference(std::string key, std::string value) {
    g_sessionPreference[std::move(key)] = std::move(value);
    const std::filesystem::path directory = cfg::dataDir();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path destination = settingsFile();
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        // 兼容旧版本文件：保留 language/theme 等非 session 行
        {
            std::ifstream existing(destination);
            std::string line;
            while (std::getline(existing, line)) {
                if (!line.starts_with("session.")) output << line << '\n';
            }
        }
        for (const auto& [key, value] : g_sessionPreference) {
            output << "session." << key << '=' << value << '\n';
        }
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(destination, ec);
        ec.clear();
        std::filesystem::rename(temporary, destination, ec);
    }
}

export std::string sessionPreference(std::string_view key) {
    if (const auto it = g_sessionPreference.find(std::string(key)); it != g_sessionPreference.end()) {
        return it->second;
    }
    return {};
}
