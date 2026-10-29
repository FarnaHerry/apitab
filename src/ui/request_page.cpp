// request_page.cpp — 请求工作区：请求树与右侧编辑/响应内容统一收进一个主卡片，
// 内部以拖拽分隔线组织区域；HTTP 可拆编辑区/响应区，WS/TCP 与 gRPC 使用整页内容。
// 发送走 store 持有的 curl 引擎（api::ApiEngine 抽象：send 纯入队，UI 协程
// PollWhile 轮询 takeResponse 取回结果，回 UI 线程写 State 并落历史）；
// 保存落到当前项目集合并刷新请求树。
// P1-C1（2026-09-02）：已按功能域拆为 request_list / request_editor /
// request_body_editor / request_response / request_tab_strip /
// environment_widgets / request_doc；本文件仅保留薄编排 RequestPage 与
// 导入接口弹窗（ApiImportDialogContent）。
#include <huxerui/huxerui.h>

#include <charconv>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ui.h"
#include "draft.h"
#include "api_import.h"
#include "app_resources.h"
#include "window_manager.h"
#include "split_pane.h"

import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.utils;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View NewRequestTypeCard(
    std::string title, std::string description, int kind, std::function<void(int)> onSelected) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    auto hovered = huxerui::UseState(false);
    auto focused = huxerui::UseState(false);
    // 入口卡常态安静（仅底色分层），悬停时出中性线提示可点；键盘聚焦交给全局焦点环。
    // 只换颜色，几何与内边距两态一致，避免悬停时内容抖动。
    const bool highlight = hovered.Get() || focused.Get();
    return huxerui::Column {
        huxerui::Text(title, huxerui::TextRole::Title).Align(huxerui::TextAlign::Center),
        huxerui::Text(std::move(description), huxerui::TextRole::Body)
            .Align(huxerui::TextAlign::Center)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = 220.0F, .height = 128.0F},
              huxerui::Background(highlight ? islands.active : islands.raised),
              huxerui::CornerRadius(islands.large_control_radius),
              huxerui::Border(hovered.Get() ? islands.line_default
                                            : huxerui::Color::Transparent(),
                              kLineWidth),
              huxerui::ClipChildren(), huxerui::Padding(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                  .label = "新建" + title})
        .OnClick([onSelected = std::move(onSelected), kind] { onSelected(kind); })
        .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& e) {
            if (e.type == huxerui::HoverEventType::Enter)
                hovered = true;
            else if (e.type == huxerui::HoverEventType::Leave)
                hovered = false;
        })
        .On<huxerui::ViewEvents::FocusChanged>(
            [focused](bool value) { focused = value; });
}

[[huxerui::composable]] huxerui::View NewRequestChooser(std::function<void(int)> onSelected) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text("新建请求", huxerui::TextRole::Title),
        huxerui::Text("选择要创建的请求类型", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        // Flow 按实际可用宽度换行；每张卡宽 220dp，不预设两列或按视口等级计算列数。
        huxerui::Flow {
            NewRequestTypeCard("HTTP 请求", "REST、JSON、表单与文件请求", 0, onSelected),
            NewRequestTypeCard("WebSocket", "长连接与实时双向消息调试", 1, onSelected),
            NewRequestTypeCard("TCP 请求", "TCP/TCPS 原始数据收发", 2, onSelected),
            NewRequestTypeCard("gRPC 请求", "RPC 接口调试（支持规划中）", 3, onSelected),
        }.With(huxerui::Spacing(theme.spacing.medium)),
    }
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::Grow(1.0F));
}

namespace {
// 与 request_editor.cpp 的 InferKvType 同逻辑，签名仅用 std 类型，可安全进头
// 但按模块约束，普通头不声明纯 helpers，此处按需并列一份；P1-C3 归并唯一实现。
std::string InferKvType(const std::string& raw) {
    const std::string s = trim(raw);
    if (s.empty()) return "string";
    if (s == "true" || s == "false") return "boolean";
    std::string_view num = s;
    if (num.front() == '+') num.remove_prefix(1);
    double parsed = 0.0;
    const auto [ptr, ec] = std::from_chars(num.data(), num.data() + num.size(), parsed,
                                           std::chars_format::general);
    if (ec == std::errc{} && ptr == num.data() + num.size()) {
        const char first = num.front();
        if ((first >= '0' && first <= '9') || first == '-' || first == '.') return "number";
    }
    return "string";
}

// api_import.h 契约的 bodyKind 字符串 → 草稿/db 的 BodyKind 下标：
// ""=0 None、json=1、text=2、xml=5、graphql=6；form=3（FormUrlEncoded——
// 导入的示例体是纯文本，url-encoded 形态最贴近，Form-Data(4) 的字段表
// 无法从一段文本还原）；其余按 text。
std::size_t ImportedBodyKindIndex(const std::string& kind) {
    if (kind == "json") return 1;
    if (kind == "xml") return 5;
    if (kind == "form") return 3;
    if (kind == "graphql") return 6;
    if (kind.empty()) return 0;
    return 2;
}

// 可调分隔：6pt 是命中热区，工作面底色填满热区以免露出 PageCard 底色；
// 中间仅绘制 kLineWidth 细线，悬停/拖动时只切换线色、不改变粗细。
// 拖动期间即使指针移出热区也保持高亮，Ended/Canceled 才熄灭。
[[huxerui::composable]] huxerui::View ResizeHandle(huxerui::Axis axis, huxerui::State<float> value,
                                                   huxerui::State<float> origin,
                                                   std::shared_ptr<SplitPaneMetrics> metrics,
                                                   bool reverse = false) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto hovered = huxerui::UseState(false);
    auto dragging = huxerui::UseState(false);
    const bool horizontal = axis == huxerui::Axis::Horizontal;
    const huxerui::Axis dragAxis = horizontal ? huxerui::Axis::Vertical
                                              : huxerui::Axis::Horizontal;
    const bool lit = hovered.Get() || dragging.Get();
    huxerui::Frame zone;
    if (horizontal)
        zone.height = SplitPane::kHandleExtent;
    else
        zone.width = SplitPane::kHandleExtent;
    const IslandTheme lines = ResolveIslandTheme(theme);
    const huxerui::Color lineColor = lit ? lines.line_focus : lines.line_subtle;
    huxerui::View line = horizontal
                             ? huxerui::View{huxerui::Row{}
                                                 .With(huxerui::Frame{.height = kLineWidth},
                                                       huxerui::Background(lineColor))}
                             : huxerui::View{huxerui::Column{}
                                                 .With(huxerui::Frame{.width = kLineWidth},
                                                       huxerui::Background(lineColor))};
    return huxerui::Stack {
               line,
           }
        .With(zone, huxerui::Background(lines.base),
              horizontal ? huxerui::Align(huxerui::HorizontalAlignment::Stretch,
                                          huxerui::VerticalAlignment::Center)
                         : huxerui::Align(huxerui::HorizontalAlignment::Center,
                                          huxerui::VerticalAlignment::Stretch),
              huxerui::PointerCursor(horizontal ? huxerui::PointerCursorKind::ResizeVertical
                                                 : huxerui::PointerCursorKind::ResizeHorizontal),
              huxerui::DragGesture{.axis = dragAxis, .minimum_distance = 0.0F})
        .On<huxerui::ViewEvents::Hover>([hovered](const huxerui::HoverEvent& event) {
            hovered = event.type != huxerui::HoverEventType::Leave;
        })
        .On<huxerui::DragEvents::Started>([origin, metrics, dragging](const huxerui::DragEvent&) {
            origin = metrics->extent;
            dragging = true;
        })
        .On<huxerui::DragEvents::Changed>([origin, value, metrics, reverse, horizontal](const huxerui::DragEvent& event) {
            const float delta = horizontal ? event.translation.y : event.translation.x;
            const float signedDelta = reverse ? -delta : delta;
            value = std::clamp(origin.Get() + signedDelta, metrics->minimum, metrics->maximum);
        })
        .On<huxerui::DragEvents::Ended>([dragging](const huxerui::DragEvent&) { dragging = false; })
        .On<huxerui::DragEvents::Canceled>([dragging](const huxerui::DragEvent&) { dragging = false; });
}

// 右侧分区与请求树统一使用 islands.base 工作面底色，无独立边框、无圆角。
// 分隔线由父布局的 ResizeHandle 绘制；高度与 Grow 由调用方设置，内容独立滚动。
huxerui::View RequestIslandSurface(huxerui::View content, const huxerui::ThemeSpec& theme,
                                   huxerui::Frame minimum = {.min_width = 320.0F,
                                                             .min_height = 140.0F}) {
    const IslandTheme islands = ResolveIslandTheme(theme);
    return std::move(content).With(huxerui::Background(islands.base),
                                   // 线条风：分区直角无边框——请求页各岛的边界由
                                   // 骨架线与拖条分隔线表达，避免相邻边框叠成双线。
                                   // SplitPane 先为对侧保留空间，再以有界约束测量本区域。
                                   minimum);
}

} // namespace

// 导入接口弹窗内容（P1-C1 起具外部链接，供请求集合树左岛“+”菜单调用；
// 原在匿名命名空间内，拆分后经 ui.h 声明跨 TU 可见。依赖的 ImportedBodyKindIndex/
// InferKvType 仍为上方匿名命名空间私有实现，本函数同 TU 内引用不受影响。）
// 导入接口对话框（DialogCard 布局，同「环境配置弹窗」的自定义内容层写法）：
// 文件来源用 SDK 公开文件选择服务 huxerui::FilePicker（UseService 取句柄 →
// OpenFileAsync → FileReference，Linux/macOS/Windows 与预编译 SDK 均有实现）。
// FileReference 是平台授予的读取能力、不暴露本地路径，故全文经
// ReadStringAsync() 读入（而非 std::ifstream）；若服务不可用（未安装——老版
// 预编译 SDK 回落，或 CanOpenFiles false）则退化为多行 TextField 粘贴。
// 解析成功后给出
// 「标题：N 个接口」+ 前几条目录/接口预览 + 「导入」；失败显示错误文本，
// 可重选文件重试。导入执行为纯同步 store 调用，完成后关闭弹窗。
[[huxerui::composable]] huxerui::View ApiImportDialogContent(huxerui::DialogContext ctx,
                                                             huxerui::State<int> listVersion) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    // UseService 未安装时抛 logic_error（老版运行时）：兜住转粘贴退化路径。
    std::shared_ptr<huxerui::FilePicker> picker;
    try {
        picker = huxerui::UseService<huxerui::FilePicker>();
    } catch (const std::exception&) {
        picker = nullptr;
    }
    const bool canPick = picker != nullptr && picker->CanOpenFiles();
    // 导入状态：fileName = 已选文件名（文件选择器不返回路径，展示名即可辨识）；
    // parsed = 解析结果（shared_ptr，空 = 尚未成功解析）；importError = 错误文本。
    auto fileName = huxerui::UseState(std::string{});
    auto parsed = huxerui::UseState(std::shared_ptr<ImportedApi>{});
    auto importError = huxerui::UseState(std::string{});
    auto pasteValue = huxerui::UseState(huxerui::TextEditingValue{});

    // 文件可很大：解析在 worker 线程运行，结果回到 UI 线程后再写 State。
    auto parseText = [tasks, parsed, importError](std::string text) {
        tasks.Launch([text = std::move(text), parsed, importError]() mutable
                         -> huxerui::Task<void> {
            auto result = co_await huxerui::RunWorker([text = std::move(text)] {
                auto api = std::make_shared<ImportedApi>();
                std::string error;
                if (!ParseApiFile(text, *api, error))
                    return std::pair{std::shared_ptr<ImportedApi>{}, std::move(error)};
                return std::pair{std::move(api), std::string{}};
            });
            if (!result.first) {
                parsed = nullptr;
                importError = std::move(result.second);
                co_return;
            }
            importError = std::string{};
            parsed = std::move(result.first);
        });
    };

    std::vector<huxerui::View> children{
        huxerui::Text("导入接口", huxerui::TextRole::Title),
    };
    if (canPick) {
        children.push_back(huxerui::Row {
            huxerui::Button("选择文件…").OnClick([tasks, picker, parseText, fileName, parsed,
                                                  importError] {
                tasks.Launch([picker, parseText, fileName, parsed,
                              importError]() -> huxerui::Task<void> {
                    const auto ref = co_await picker->OpenFileAsync(
                        huxerui::FilePickerFilter{.name = "接口文件 (JSON)",
                                                  .extensions = {"json", "yaml", "yml"}});
                    if (!ref.has_value()) co_return; // 用户取消
                    auto read = co_await ref->ReadStringAsync();
                    if (!read.Succeeded()) {
                        parsed = nullptr;
                        importError = "读取文件失败: " + read.Error().message;
                        co_return;
                    }
                    fileName = ref->Name();
                    parseText(read.Value());
                });
            }),
            huxerui::Text(fileName.Get().empty() ? "未选择文件" : fileName.Get(),
                          huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant),
                      huxerui::Grow(1.0F), huxerui::ClipChildren()),
        }
                      .With(huxerui::Spacing(theme.spacing.small),
                            huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
    } else {
        // 无文件选择能力（如受限平台）：退化为粘贴文件全文再解析。
        children.push_back(huxerui::TextField(pasteValue.Get())
                               .Label("文件内容（JSON）")
                               .Placeholder("粘贴 Swagger/OpenAPI 或 Postman Collection 全文")
                               .Variant(huxerui::TextFieldVariant::Outlined)
                               .LineLimits(huxerui::TextFieldLineLimits::MultiLine(6))
                               .OnChanged([pasteValue](const huxerui::TextEditingValue& value) {
                                   pasteValue = value;
                               }));
        children.push_back(huxerui::Row {
            PrimaryButton(huxerui::Button("解析粘贴内容").OnClick([pasteValue, parseText] {
                parseText(pasteValue.Get().text);
            })),
        });
    }
    if (!importError.Get().empty()) {
        children.push_back(huxerui::Text(importError.Get(), huxerui::TextRole::Body)
                               .With(huxerui::Foreground(theme.colors.error)));
    }
    // 预览：标题 + 接口总数 + 前 6 条「目录 · 方法 URL — 名称」。
    if (const auto api = parsed.Get(); api != nullptr) {
        children.push_back(huxerui::Text(
            api->title + "：" + std::to_string(api->operations.size()) + " 个接口",
            huxerui::TextRole::Body));
        std::vector<huxerui::View> lines;
        for (std::size_t i = 0; i < api->operations.size() && i < 6; ++i) {
            const ImportedOperation& op = api->operations[i];
            std::string dir;
            for (const std::string& seg : op.dirChain) {
                if (!dir.empty()) dir += '/';
                dir += seg;
            }
            std::string line = (dir.empty() ? "（根）" : dir) + " · " + op.method + " " + op.url;
            if (!op.name.empty()) line += " — " + op.name;
            lines.push_back(huxerui::Text(std::move(line), huxerui::TextRole::Label)
                                .With(huxerui::Foreground(theme.colors.on_surface_variant),
                                      huxerui::ClipChildren()));
        }
        if (api->operations.size() > 6) {
            lines.push_back(huxerui::Text(
                "…（其余 " + std::to_string(api->operations.size() - 6) + " 条略）",
                huxerui::TextRole::Label)
                                .With(huxerui::Foreground(theme.colors.on_surface_variant)));
        }
        children.push_back(huxerui::Column(std::move(lines))
                               .With(huxerui::Spacing(2.0F),
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
    }
    children.push_back(huxerui::Row {
        huxerui::Button("取消").OnClick([ctx] { ctx.Dismiss(); }),
        PrimaryButton(huxerui::Button("导入").OnClick([ctx, toast, listVersion, parsed] {
            const auto api = parsed.Get();
            if (api == nullptr) {
                toast.Show("请先选择文件并成功解析");
                return;
            }
            // dirChain 逐级 find-or-create 分组（Name 模式，仅组织作用、不参与 URL），
            // 每条 op 组装 db::SavedRequest 落库；遇错即停并保留弹窗供用户查看提示。
            std::size_t count = 0;
            for (const ImportedOperation& op : api->operations) {
                std::int64_t parent = 0;
                bool failed = false;
                for (const std::string& seg : op.dirChain) {
                    auto findChild = [&](std::int64_t parentId) {
                        std::int64_t gid = 0;
                        for (const db::Group& g : g_requests.groups()) {
                            if (g.parentId == parentId && g.name == seg) {
                                gid = g.id;
                                break;
                            }
                        }
                        return gid;
                    };
                    std::int64_t gid = findChild(parent);
                    if (gid == 0) {
                        if (auto result = g_requests.createGroup(
                                seg, db::GroupMode::Name, parent);
                            !result) {
                            toast.Show("导入失败: " + result.error().message);
                            failed = true;
                            break;
                        }
                        gid = findChild(parent); // store 建后 reload，按名回查 id
                        if (gid == 0) {
                            toast.Show("导入失败: 新建目录回查不到");
                            failed = true;
                            break;
                        }
                    }
                    parent = gid;
                }
                if (failed) return;
                db::SavedRequest rec;
                rec.groupId = parent;
                rec.name = op.name.empty() ? op.method + " " + op.url : op.name;
                rec.method = op.method; // 原样字符串（保存/发送接受任意方法名）
                rec.url = op.url;       // fullUrl 原样（含 scheme，finalizeSpec 不再
                                        // 拼环境）；否则为 path 相对
                for (const ImportedParam& p : op.params) {
                    rec.params.push_back(api::KeyValue{.key = p.key, .value = p.value,
                                                       .enabled = true,
                                                       .type = InferKvType(p.value),
                                                       .remark = p.remark});
                }
                for (const ImportedParam& h : op.headers) {
                    rec.headers.push_back(api::KeyValue{.key = h.key, .value = h.value,
                                                        .enabled = true,
                                                        .type = InferKvType(h.value),
                                                        .remark = h.remark});
                }
                const std::size_t bk = ImportedBodyKindIndex(op.bodyKind);
                rec.bodyKind = static_cast<api::BodyKind>(bk);
                rec.body = op.body; // 兼容字段：当前类型文本
                if (bk < rec.bodyContents.size()) rec.bodyContents[bk].text = op.body;
                if (auto result = g_requests.save(rec); !result) {
                    toast.Show("导入失败: " + result.error().message);
                    return;
                }
                ++count;
            }
            toast.Show(std::format("已导入 {} 个接口", count));
            listVersion = listVersion.Get() + 1;
            ctx.Dismiss();
        })),
    }
                      .With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)));
    return DialogCard(huxerui::Column(std::move(children))
                          .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 520.0F},
                                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View RequestPage(huxerui::State<std::int64_t> opened,
                                                   std::int64_t windowId) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    // 岛屿几何令牌：岛内边距统一从这里取（工具化密度 = small/8pt）。
    // 局部 islands 变量是右侧分支的 View 容器，与令牌命名避让。
    const IslandTheme islandTheme = ResolveIslandTheme(theme);

    // 未打开项目的兜底已删：主页整宽覆盖侧栏后本页在 opened==0 时不可达。
    // 仍订阅 opened：切换项目时，保活的请求页需要重组一次，让环境选择器从
    // 当前项目游标重新读取并显示对应环境。
    (void)opened.Get();

    // 多窗口信封：拆出/拖回时项目标签随窗口迁移，未保存草稿经 WindowManager
    // 的转移信封在首组合恢复（按当前项目游标匹配；重组时 UseState 丢弃初值，
    // 重复消费返回空、无害）。
    const auto windowManager = huxerui::UseService<WindowManager>();
    std::vector<RequestDraft> restoredDrafts;
    if (auto workspace = windowManager->ConsumeWorkspace(g_requests.currentProjectId())) {
        restoredDrafts = std::move(workspace->drafts);
    }

    // 内部标签页：每个打开的请求一个草稿；响应区状态为页面级（单引擎）。
    auto openDrafts = huxerui::UseState<std::vector<RequestDraft>>(std::move(restoredDrafts));
    auto activeTab = huxerui::UseState<std::size_t>(0);
    auto newTabOpen = huxerui::UseState(false);
    auto listVersion = huxerui::UseState(0);
    // 环境版本：环境选择/CRUD/弹窗保存后 bump，标签条与环境弹窗按它重读 store。
    auto envVersion = huxerui::UseState(0);
    // 编辑器子页：0=调试 1=文档 2=测试用例 3=Mock（仅 HTTP 草稿用；非调试页时
    // 响应区隐藏，编辑器取得整个右侧区域）。
    auto editorPage = huxerui::UseState<std::size_t>(0);
    auto inFlight = huxerui::UseState(false);
    auto responseBody = huxerui::UseState(std::string{"（尚未发送请求）"});
    auto responseHeaders = huxerui::UseStateList<std::string>();
    auto responseCookies = huxerui::UseStateList<std::string>();
    // HTML 预览文档（原文 + base URL + 代数）：编辑器发送路径写入，响应区
    // Preview 标签消费。
    auto responsePreview = huxerui::UseState(ResponsePreview{});
    auto leftIslandWidth = huxerui::UseState(260.0F);
    auto leftIslandOrigin = huxerui::UseState(260.0F);
    auto responseIslandHeight = huxerui::UseState(260.0F);
    auto responseIslandOrigin = huxerui::UseState(260.0F);
    const auto leftMetrics = huxerui::UseState(std::make_shared<SplitPaneMetrics>());
    const auto responseMetrics = huxerui::UseState(std::make_shared<SplitPaneMetrics>());

    // 多窗口快照采集：拆出/拖回的发起窗口侧经 WindowManager 采集本页草稿
    //（nav_page 由根组合另行提供）。挂载注册、卸载注销；本窗口关闭时清理
    // 随协程帧走，指针不悬挂。
    huxerui::Lifecycle(
        [windowManager, windowId, openDrafts, activeTab] {
            windowManager->RegisterCapture(windowId, [openDrafts, activeTab] {
                return WorkspaceSnapshot{openDrafts.Get(), activeTab.Get(), 0};
            });
            return [windowManager, windowId] { windowManager->UnregisterCapture(windowId); };
        },
        windowId);

    // 窄桌面仍保留左右工作区，收窄请求树；避免上下堆叠挤占 HTTP 编辑/响应区高度。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    const std::vector<RequestDraft> snapshot = openDrafts.Get();
    const std::size_t current =
        snapshot.empty() ? 0 : std::min(activeTab.Get(), snapshot.size() - 1);
    const int currentKind = snapshot.empty() ? 0 : snapshot[current].kind;
    // 右岛内容 Key：kind + 标签下标组合，切标签/切类型时正确重组。
    const std::int64_t contentKey = static_cast<std::int64_t>(current) * 4 + currentKind;

    // 右侧按活跃草稿类型分派：
    // - HTTP 调试页：上 = 标签条 + 编辑器（Grow 1，取得剩余高度），下 =
    //   响应区（高度 State 可拖 140–640dp），中间独立 6dp 拖条；编辑器固定部分
    //   （名称/操作栏/分区切换/Body 类型行）不滚动，分区内容与响应各自内滚。
    // - WS/TCP：直接嵌入会话组件，顶部操作区固定、事件流独立滚动，不再外套 ScrollView；
    //   会话生命周期由各组件自己的组合状态和事件泵持有。
    // - gRPC（kind=3）：引擎未实现——整页占位（仅标签条 + 提示文案，无操作栏、
    //   无分区、不可保存/发送；gRPC 草稿不落库）。
    // - 无打开草稿或新建标签：请求类型选择器。
    huxerui::View rightArea = huxerui::Column{};
    if (snapshot.empty() || newTabOpen.Get()) {
        auto createDraft = [openDrafts, activeTab, newTabOpen](int kind) {
            std::vector<RequestDraft> copy = openDrafts.Get();
            copy.push_back(RequestDraft{.kind = kind});
            openDrafts = copy;
            activeTab = copy.size() - 1;
            newTabOpen = false;
        };
        rightArea = RequestIslandSurface(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
                NewRequestChooser(createDraft),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                      huxerui::Padding(islandTheme.island_padding),
                      huxerui::Grow(1.0F)),
            theme);
    } else if (currentKind == 1 || currentKind == 2) {
        huxerui::View content = currentKind == 1
                                    ? WebSocketPage().With(huxerui::Grow(1.0F)).Key(contentKey)
                                    : TcpPage().With(huxerui::Grow(1.0F)).Key(contentKey);
        rightArea = RequestIslandSurface(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
                content,
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Padding(islandTheme.island_padding),
                      huxerui::Grow(1.0F)),
            theme);
    } else if (currentKind == 3) {
        // gRPC 占位页：支持规划中——只给标签条 + 居中提示，无操作栏/分区。
        rightArea = RequestIslandSurface(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
                huxerui::Column {
                    huxerui::Text("gRPC 请求：支持规划中（暂不可保存/发送）",
                                  huxerui::TextRole::Title),
                    huxerui::Text("协议引擎尚未接入，本标签仅作占位，可直接关闭。",
                                  huxerui::TextRole::Body)
                        .With(huxerui::Foreground(theme.colors.on_surface_variant)),
                }
                    .With(huxerui::Spacing(theme.spacing.small),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                          huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                          huxerui::Grow(1.0F))
                    .Key(contentKey),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Padding(islandTheme.island_padding),
                      huxerui::Grow(1.0F)),
            theme);
    } else {
        // 非调试子页隐藏响应区；调试页由编辑器 Grow 分配响应区之外的剩余高度。
        // 两个无边框分区之间插入 6dp 拖拽热区，中间仅绘制 kLineWidth 单线。
        const bool debugging = editorPage.Get() == 0;
        std::vector<huxerui::View> islands;
        // HTTP 编辑器岛（含 URL 栏）比普通响应/会话岛需要更高的最低工作区，
        // 保证标签、名称、URL 操作栏与一个可编辑分区能够同时保留。
        islands.push_back(RequestIslandSurface(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
                RequestEditor(openDrafts, current, activeTab, listVersion,
                              inFlight, responseBody, responseHeaders,
                              responseCookies, responsePreview, envVersion, editorPage)
                    .Key(contentKey)
                    .With(huxerui::Grow(1.0F)),
            }
                .With(huxerui::Spacing(theme.spacing.small),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Padding(islandTheme.island_padding),
                      huxerui::Grow(1.0F)),
            theme, huxerui::Frame{.min_width = 480.0F, .min_height = 300.0F}));
        if (debugging) {
            islands.push_back(ResizeHandle(huxerui::Axis::Horizontal, responseIslandHeight,
                                           responseIslandOrigin, responseMetrics.Get(), true));
            islands.push_back(RequestIslandSurface(
                huxerui::Column {
                    ResponseArea(responseBody, responseHeaders, responseCookies,
                                 responsePreview, inFlight, theme)
                        .With(huxerui::Grow(1.0F)),
                }
                    .With(huxerui::Spacing(theme.spacing.small),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                          huxerui::Padding(islandTheme.island_padding)),
                theme));
        }
        rightArea = debugging
            ? huxerui::View{SplitPane(std::move(islands))
                  .LayoutValue<SplitPaneOptions>(SplitPaneOptions{
                      .axis = huxerui::Axis::Vertical,
                      .extent = responseIslandHeight.Get(),
                      .minimum = 140.0F,
                      .maximum = 640.0F,
                      .reserve = 300.0F,
                      .trailing = true,
                      .metrics = responseMetrics.Get(),
                  }).With(huxerui::Grow(1.0F))}
            : islands.front();
    }

    // 在父约束内为编辑区保留 480dp，拖动与缩窗都不能把请求树扩到编辑区之上。
    huxerui::View workspace = SplitPane {
      RequestListIsland(openDrafts, activeTab, listVersion, false, leftIslandWidth.Get()),
      ResizeHandle(huxerui::Axis::Vertical, leftIslandWidth, leftIslandOrigin, leftMetrics.Get()),
      rightArea,
    }.LayoutValue<SplitPaneOptions>(SplitPaneOptions{
        .extent = leftIslandWidth.Get(),
        .maximum = compact ? 220.0F : 420.0F,
        .metrics = leftMetrics.Get(),
    }).With(huxerui::Grow(1.0F));
    // 请求页本身只绘制内部工作区分隔线；四边外框由应用路由层的 PageCard 统一提供。
    return std::move(workspace);
}

} // namespace apitab::ui
