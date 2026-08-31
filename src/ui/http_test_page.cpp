// http_test_page.cpp — 框架 HTTP 协程压测实验页：不走 curl 引擎，直接用 HuxerUI
// 自带的 HttpClient 服务（UseService<HttpClient>()）+ UseTaskScope 协程，按输入的
// 地址/方法/并发数起 N 个协程连续发请求，实时统计成功/失败/耗时，用于观察并发到
// 多少协程时 UI 开始卡。
// 线程模型：HttpClient::Send 的 continuation 回到 UI 线程（框架文档约定），所以
// StatsCell 的计数直接在协程里自增，无需原子量；展示层由 0.5s Ticker 拷进 State。
#include <huxerui/huxerui.h>

#include <array>
#include <chrono>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "ui.h"

namespace apitab::ui {

namespace {

// 压测共享计数：只在 UI 线程读写（Send continuation 与 Ticker 都在 UI 线程）。
struct StatsCell {
    bool running = false;
    int activeCoroutines = 0;
    long long done = 0;
    long long ok = 0;
    long long failed = 0;
    double totalMs = 0.0;
    std::string lastError;
    std::chrono::steady_clock::time_point startedAt;
};

// 框架 HttpClient 的 huxerui::HttpMethod 枚举只有经典 7 个方法（QUERY/CONNECT/
// WebDAV 等发不了），本页选择列表因此不扩充——请求页/k6 压测页已是 20 个。
constexpr std::array<std::string_view, 7> kHttpMethodNames{
    "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};

// 下标与 kHttpMethodNames 一一对应（huxerui::HttpMethod 同序）。
huxerui::HttpMethod HttpMethodForIndex(std::size_t index) {
    switch (index) {
        case 1: return huxerui::HttpMethod::Head;
        case 2: return huxerui::HttpMethod::Post;
        case 3: return huxerui::HttpMethod::Put;
        case 4: return huxerui::HttpMethod::Patch;
        case 5: return huxerui::HttpMethod::Delete;
        case 6: return huxerui::HttpMethod::Options;
        default: return huxerui::HttpMethod::Get;
    }
}

} // namespace

[[huxerui::composable]] huxerui::View HttpTestPage() {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // 组合期取服务句柄（文档约定：先入句柄再 Launch，悬挂后不再查服务）。
    auto http = huxerui::UseService<huxerui::HttpClient>();

    auto url = huxerui::UseState(huxerui::TextEditingValue{"https://"});
    auto methodIndex = huxerui::UseState(std::size_t{0});
    auto concurrency = huxerui::UseState(huxerui::TextEditingValue{"50"});
    auto cell = huxerui::UseState(std::make_shared<StatsCell>());
    // Ticker 节拍：压测中每 0.5s bump 一次触发重组，把 cell 快照刷到界面上；
    // 不按请求逐个写 State（高 RPS 下每次重组太贵）。
    auto statsTick = huxerui::UseState(0);
    (void)statsTick.Get(); // 订阅节拍
    const StatsCell snapshot = *cell.Get();

    auto stopAll = [cell] { cell.Get()->running = false; };
    auto startAll = [=] {
        const std::string target = url.Get().text;
        if (target.size() <= 8) { // "https://" / "http://" 之外没内容
            toast.Show("请先填写完整请求地址（含 http/https）");
            return;
        }
        int n = 0;
        try {
            n = std::stoi(concurrency.Get().text);
        } catch (...) {
            n = 0;
        }
        // 不设上限：这页本来就是用来观察并发到多少协程时 UI 开始卡的。
        if (n < 1) {
            toast.Show("并发协程数需为正整数");
            return;
        }
        if (cell.Get()->running) return;
        *cell.Get() = StatsCell{.running = true,
                                .startedAt = std::chrono::steady_clock::now()};
        const huxerui::HttpMethod method = HttpMethodForIndex(methodIndex.Get());
        for (int i = 0; i < n; ++i) {
            tasks.Launch([http, cell, target, method]() -> huxerui::Task<void> {
                cell.Get()->activeCoroutines++;
                // 连续发直到停止：单请求 10s 超时兜底，失败也计入统计继续转。
                while (cell.Get()->running) {
                    const auto t0 = std::chrono::steady_clock::now();
                    huxerui::HttpResult result = co_await http->Send(
                        huxerui::HttpRequest{
                            .url = target,
                            .method = method,
                            .timeout = std::chrono::milliseconds{10000},
                        });
                    const double ms =
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
                    StatsCell& c = *cell.Get();
                    c.done++;
                    c.totalMs += ms;
                    if (result.HasResponse()) {
                        c.ok++;
                    } else {
                        c.failed++;
                        c.lastError = result.Error().message;
                    }
                }
                cell.Get()->activeCoroutines--;
            });
        }
        // 节拍器：运行期间每 0.5s 触发一次重组刷新统计。
        tasks.Launch([cell, statsTick]() -> huxerui::Task<void> {
            while (cell.Get()->running) {
                co_await huxerui::Delay(std::chrono::duration<double>{0.5});
                statsTick = statsTick.Get() + 1;
            }
            statsTick = statsTick.Get() + 1; // 收尾再刷一次
        });
    };

    // 展示值：平均耗时 / 运行秒数 / 实时 RPS。
    const double avgMs = snapshot.done > 0 ? snapshot.totalMs / snapshot.done : 0.0;
    const double elapsed =
        snapshot.done > 0 || snapshot.running
            ? std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                            snapshot.startedAt)
                  .count()
            : 0.0;
    const double rps = elapsed > 0.0 ? snapshot.done / elapsed : 0.0;
    const auto statText = [&theme](std::string text) {
        return huxerui::Text(std::move(text), huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant));
    };

    return huxerui::Column {
        PageHeader("框架 HTTP 协程压测", "HuxerUI 内置 HttpClient + 协程，不走 curl 引擎"),
        // URL 独立整行输入（醒目）；方法/并发数/开始按钮在下一行。
        huxerui::TextField(url.Get())
            .Label("请求地址")
            .Placeholder("https://api.example.com/ping")
            .Variant(huxerui::TextFieldVariant::Outlined)
            .OnChanged([url](const huxerui::TextEditingValue& value) { url = value; }),
        huxerui::Row {
            huxerui::Select(std::vector<std::string>(kHttpMethodNames.begin(),
                                                     kHttpMethodNames.end()),
                            methodIndex.Get(),
                            // 选项与触发器显示复用同一工厂，统一按 MethodColor
                            // 色表逐方法着色。工厂会被菜单层稍后调用，颜色在
                            // 组合期解析成快照按值捕获，不按引用捕获 theme。
                            [colors = [&theme] {
                                 std::vector<huxerui::Color> v;
                                 v.reserve(kHttpMethodNames.size());
                                 for (const std::string_view m : kHttpMethodNames)
                                     v.push_back(MethodColor(theme, m));
                                 return v;
                             }()](const std::string& option) -> huxerui::View {
                                huxerui::Color color = colors.back();
                                for (std::size_t i = 0; i < kHttpMethodNames.size(); ++i)
                                    if (kHttpMethodNames[i] == option) {
                                        color = colors[i];
                                        break;
                                    }
                                return huxerui::Text(option)
                                    .With(huxerui::Foreground(color))
                                    .Key(option);
                            })
                .OnChanged([methodIndex](std::size_t index) { methodIndex = index; })
                // Select 内部 trigger 带 Grow，loose 测量下会吃满整行挤出后面的
                // 输入框和按钮；用固定宽度约束住。
                .With(huxerui::Frame{.width = 140.0F}),
            huxerui::Text("并发协程数", huxerui::TextRole::Label),
            huxerui::TextField(concurrency.Get())
                .Variant(huxerui::TextFieldVariant::Outlined)
                .OnChanged([concurrency](const huxerui::TextEditingValue& value) {
                    concurrency = value;
                })
                .With(huxerui::Frame{.width = 120.0F}),
            snapshot.running
                ? huxerui::View{huxerui::Button("停止").OnClick([stopAll] { stopAll(); })}
                : huxerui::View{huxerui::Button("开始压测").OnClick([startAll] { startAll(); })},
        }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
        huxerui::Text(
            std::format("协程：{} 个在跑 · 完成 {}（成功 {} / 失败 {}）· 平均 {:.1f} ms · {:.1f} req/s",
                        snapshot.activeCoroutines, snapshot.done, snapshot.ok, snapshot.failed,
                        avgMs, rps),
            huxerui::TextRole::Label),
        snapshot.lastError.empty()
            ? huxerui::View{statText("暂无错误")}
            : huxerui::View{statText("最近错误：" + snapshot.lastError)},
        statText("提示：并发拉高后观察界面是否掉帧/卡顿；停止由协程循环标志控制，"
                 "进行中的请求最多再等 10s 超时。"),
    }
        .With(huxerui::Padding(theme.spacing.large),
              huxerui::Spacing(theme.spacing.medium),
              huxerui::Background(theme.colors.surface_container_low),
              huxerui::CornerRadius(theme.shapes.large), huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
