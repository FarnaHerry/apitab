// i18n.cppm — 应用文案与语言偏好；不依赖 EUI 或领域 store。
export module apitab.i18n;

import std;
import apitab.config;

export enum class Language { Chinese, English };

export Language g_language = Language::Chinese;
// ThemeMode lives in the UI theme module; keep the persisted value primitive here
// so the preference layer remains independent of EUI and theme rendering.
export int g_savedThemeMode = 2;

export enum class UiText {
    AppTitle,
    Settings,
    GlobalSettings,
    Theme,
    Language,
    Dark,
    Light,
    System,
    ThemeHint,
    Chinese,
    English,
    Save,
    Cancel,
    Confirm,
    Delete,
    Connect,
    Disconnect,
    Connected,
    Connecting,
    Disconnected,
    ConnectionFailed,
    RequestCollection,
    NewRequest,
    HttpRequest,
    OtherRequestTypes,
    WebSocket,
    Tcp,
    RequestName,
    RequestNameRequired,
    SaveFailed,
    Saved,
    NoEnvironment,
    ManageEnvironments,
    ProjectSettings,
    Home,
    History,
    LoadTest,
    StartLoadTest,
    Stop,
    HttpOnlyLoad,
    UrlRequired,
    Send,
    SendData,
    Text,
    Binary,
    Hex,
    ReceiveText,
    ReceiveHex,
    NoEvents,
    TcpConnecting,
    TcpAddressRequired,
    TcpAddressInvalid,
    TcpNotConnected,
};

namespace {

std::string_view chinese(UiText key) {
    switch (key) {
        case UiText::AppTitle: return "apitab — API 测试与压测";
        case UiText::Settings: return "设置";
        case UiText::GlobalSettings: return "基础设置";
        case UiText::Theme: return "外观主题";
        case UiText::Language: return "语言";
        case UiText::Dark: return "深色模式";
        case UiText::Light: return "浅色模式";
        case UiText::System: return "跟随系统";
        case UiText::ThemeHint: return "跟随系统会在选择时读取当前桌面可用的默认主题。";
        case UiText::Chinese: return "中文";
        case UiText::English: return "English";
        case UiText::Save: return "保存";
        case UiText::Cancel: return "取消";
        case UiText::Confirm: return "确定";
        case UiText::Delete: return "删除";
        case UiText::Connect: return "连接";
        case UiText::Disconnect: return "断开";
        case UiText::Connected: return "已连接";
        case UiText::Connecting: return "连接中";
        case UiText::Disconnected: return "未连接";
        case UiText::ConnectionFailed: return "连接失败";
        case UiText::RequestCollection: return "请求集合";
        case UiText::NewRequest: return "新建请求";
        case UiText::HttpRequest: return "HTTP 请求";
        case UiText::OtherRequestTypes: return "其他请求类型";
        case UiText::WebSocket: return "WebSocket";
        case UiText::Tcp: return "TCP";
        case UiText::RequestName: return "请求名称";
        case UiText::RequestNameRequired: return "请求名称不能为空";
        case UiText::SaveFailed: return "保存失败";
        case UiText::Saved: return "已保存";
        case UiText::NoEnvironment: return "未选择环境";
        case UiText::ManageEnvironments: return "环境管理";
        case UiText::ProjectSettings: return "项目设置";
        case UiText::Home: return "主页";
        case UiText::History: return "历史记录";
        case UiText::LoadTest: return "压测";
        case UiText::StartLoadTest: return "开始压测";
        case UiText::Stop: return "停止";
        case UiText::HttpOnlyLoad: return "只有 HTTP 请求支持 k6 压测";
        case UiText::UrlRequired: return "URL 不能为空";
        case UiText::Send: return "发送";
        case UiText::SendData: return "发送数据";
        case UiText::Text: return "文本";
        case UiText::Binary: return "二进制";
        case UiText::Hex: return "Hex";
        case UiText::ReceiveText: return "接收文本";
        case UiText::ReceiveHex: return "接收 Hex";
        case UiText::NoEvents: return "连接后将在此显示事件和消息";
        case UiText::TcpConnecting: return "TCP 连接中…";
        case UiText::TcpAddressRequired: return "TCP 地址不能为空";
        case UiText::TcpAddressInvalid: return "TCP 地址无效";
        case UiText::TcpNotConnected: return "TCP 尚未连接";
    }
    return {};
}

std::string_view english(UiText key) {
    switch (key) {
        case UiText::AppTitle: return "apitab — API Testing & Load Testing";
        case UiText::Settings: return "Settings";
        case UiText::GlobalSettings: return "General Settings";
        case UiText::Theme: return "Theme";
        case UiText::Language: return "Language";
        case UiText::Dark: return "Dark";
        case UiText::Light: return "Light";
        case UiText::System: return "System";
        case UiText::ThemeHint: return "System reads the current desktop preference when selected.";
        case UiText::Chinese: return "Chinese";
        case UiText::English: return "English";
        case UiText::Save: return "Save";
        case UiText::Cancel: return "Cancel";
        case UiText::Confirm: return "Confirm";
        case UiText::Delete: return "Delete";
        case UiText::Connect: return "Connect";
        case UiText::Disconnect: return "Disconnect";
        case UiText::Connected: return "Connected";
        case UiText::Connecting: return "Connecting";
        case UiText::Disconnected: return "Disconnected";
        case UiText::ConnectionFailed: return "Connection Failed";
        case UiText::RequestCollection: return "Requests";
        case UiText::NewRequest: return "New Request";
        case UiText::HttpRequest: return "HTTP Request";
        case UiText::OtherRequestTypes: return "Other Request Types";
        case UiText::WebSocket: return "WebSocket";
        case UiText::Tcp: return "TCP";
        case UiText::RequestName: return "Request Name";
        case UiText::RequestNameRequired: return "Request name is required";
        case UiText::SaveFailed: return "Save failed";
        case UiText::Saved: return "Saved";
        case UiText::NoEnvironment: return "No environment";
        case UiText::ManageEnvironments: return "Manage Environments";
        case UiText::ProjectSettings: return "Project Settings";
        case UiText::Home: return "Home";
        case UiText::History: return "History";
        case UiText::LoadTest: return "Load Test";
        case UiText::StartLoadTest: return "Start Load Test";
        case UiText::Stop: return "Stop";
        case UiText::HttpOnlyLoad: return "Only HTTP requests support k6 load tests";
        case UiText::UrlRequired: return "URL is required";
        case UiText::Send: return "Send";
        case UiText::SendData: return "Send Data";
        case UiText::Text: return "Text";
        case UiText::Binary: return "Binary";
        case UiText::Hex: return "Hex";
        case UiText::ReceiveText: return "Receive Text";
        case UiText::ReceiveHex: return "Receive Hex";
        case UiText::NoEvents: return "Events and messages appear here after connecting";
        case UiText::TcpConnecting: return "TCP connecting…";
        case UiText::TcpAddressRequired: return "TCP address is required";
        case UiText::TcpAddressInvalid: return "Invalid TCP address";
        case UiText::TcpNotConnected: return "TCP is not connected";
    }
    return {};
}

std::filesystem::path settingsFile() { return cfg::dataDir() / "settings.ini"; }

} // namespace

export std::string tr(UiText key) {
    return std::string(g_language == Language::English ? english(key) : chinese(key));
}

export std::vector<std::string> languageNames() {
    return {tr(UiText::Chinese), tr(UiText::English)};
}

export void loadLanguagePreference() {
    std::ifstream input(settingsFile());
    if (!input) return;
    std::string line;
    while (std::getline(input, line)) {
        if (line == "language=en-US") g_language = Language::English;
        else if (line == "language=zh-CN") g_language = Language::Chinese;
        else if (line.starts_with("theme=")) {
            try {
                const int value = std::stoi(line.substr(6));
                if (value >= 0 && value <= 2) g_savedThemeMode = value;
            } catch (...) {
                g_savedThemeMode = 2;
            }
        }
    }
}

export void saveLanguagePreference() {
    const std::filesystem::path directory = cfg::dataDir();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path destination = settingsFile();
    const std::filesystem::path temporary = destination.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return;
        output << "language=" << (g_language == Language::English ? "en-US" : "zh-CN") << '\n';
        output << "theme=" << std::clamp(g_savedThemeMode, 0, 2) << '\n';
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(destination, ec);
        ec.clear();
        std::filesystem::rename(temporary, destination, ec);
    }
}

export void setLanguage(Language language) {
    if (g_language == language) return;
    g_language = language;
    saveLanguagePreference();
}
