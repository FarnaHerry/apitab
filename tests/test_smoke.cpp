// Smoke test — verifies the project compiles + a binary runs.
// Add more tests as tests/test_*.cpp files; mcpp test discovers them
// automatically (one binary per file).
import std;
import apitab.utils;

int main() {
    const auto parsed = parseUrlQuery(
        "https://example.com/search?q=hello+world&lang=zh%2DCN#ignored");
    if (!parsed.intercepted || parsed.url != "https://example.com/search" ||
        parsed.params != std::vector<std::pair<std::string, std::string>>{
            {"q", "hello world"}, {"lang", "zh-CN"}}) {
        std::println(std::cerr, "test_smoke: URL query parsing failed");
        return 1;
    }
    const auto noQuery = parseUrlQuery("https://example.com/search");
    if (noQuery.intercepted || noQuery.url != "https://example.com/search" ||
        !noQuery.params.empty()) {
        std::println(std::cerr, "test_smoke: URL without query changed unexpectedly");
        return 1;
    }
    std::println("test_smoke: ok");
    return 0;
}
