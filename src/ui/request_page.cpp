// request_page.cpp — 请求工作区：左岛 = 当前项目的请求列表；右侧 HTTP 拆上下两岛
// （上 = 标签条 + 编辑器，下 = 响应区；WS/TCP 自包含整页仍单岛；gRPC 单岛占位）。
// 发送走 store 持有的 curl 引擎（api::ApiEngine 抽象：send 纯入队，UI 协程
// PollWhile 轮询 takeResponse 取回结果，回 UI 线程写 State 并落历史）；
// 保存落到当前项目集合并刷新左岛列表。
// P1-C1（2026-09-02）：已按功能域拆为 request_list / request_editor /
// request_body_editor / request_response / request_tab_strip /
// environment_widgets / request_doc；本文件仅保留薄编排 RequestPage 与
// 导入接口弹窗（ApiImportDialogContent）。
#include <huxerui/huxerui.h>

#include <charconv>
#include <string>
#include <vector>

#include "ui.h"
#include "draft.h"
#include "api_import.h"
#include "app_resources.h"

import apitab.api_engine;
import apitab.db;
import apitab.store.requests;
import apitab.utils;

namespace apitab::ui {

[[huxerui::composable]] huxerui::View NewRequestTypeCard(
    std::string title, std::string description, int kind, std::function<void(int)> onSelected) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    const IslandTheme islands = ResolveIslandTheme(theme);
    return huxerui::Column {
        huxerui::Text(title, huxerui::TextRole::Title).Align(huxerui::TextAlign::Center),
        huxerui::Text(std::move(description), huxerui::TextRole::Body)
            .Align(huxerui::TextAlign::Center)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
    }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Frame{.width = 220.0F, .height = 128.0F},
              huxerui::Background(islands.raised),
              huxerui::CornerRadius(islands.large_control_radius),
              huxerui::Border(islands.outline_soft, 1.0F),
              huxerui::ClipChildren(), huxerui::Padding(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
              huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
              huxerui::Focusable(true),
              huxerui::Semantics{.role = huxerui::SemanticRole::Button,
                                  .label = "新建" + title})
        .OnClick([onSelected = std::move(onSelected), kind] { onSelected(kind); });
}

[[huxerui::composable]] huxerui::View NewRequestChooser(std::function<void(int)> onSelected) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    return huxerui::Column {
        huxerui::Text("新建请求", huxerui::TextRole::Title),
        huxerui::Text("选择要创建的请求类型", huxerui::TextRole::Body)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)),
        huxerui::Row {
            NewRequestTypeCard("HTTP 请求", "REST、JSON、表单与文件请求", 0, onSelected),
            NewRequestTypeCard("WebSocket", "长连接与实时双向消息调试", 1, onSelected),
        }.With(huxerui::Spacing(theme.spacing.medium)),
        huxerui::Row {
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

// Chrome 风格的连续标签轮廓：外层轮廓只负责绘制表面和边界，实际 Tabs
// 与页面内容保持透明并叠在 Canvas 之上。选中标签的顶部使用一个浅拱形连接，
// 看起来像浏览器标签页而不是一组彼此分离的卡片。
huxerui::Path MakeConnectedTabOutline(huxerui::Size size, std::size_t selectedIndex) {
    const float w = size.width;
    const float h = size.height;
    const float radius = 12.0F;
    const float stride = 160.0F + 1.0F + 2.0F * 8.0F;
    const float tabStart = std::clamp(static_cast<float>(selectedIndex) * stride, 0.0F,
                                      std::max(0.0F, w - 160.0F));
    const float tabEnd = std::min(w, tabStart + 160.0F);
    const float bump = std::min(10.0F, std::max(4.0F, h * 0.08F));
    return huxerui::Path()
        .MoveTo({radius, 0.0F})
        .LineTo({tabStart, 0.0F})
        .CubicTo({tabStart + 4.0F, 0.0F}, {tabStart + 5.0F, bump},
                 {tabStart + 12.0F, bump})
        .LineTo({tabEnd - 12.0F, bump})
        .CubicTo({tabEnd - 5.0F, bump}, {tabEnd - 4.0F, 0.0F}, {tabEnd, 0.0F})
        .LineTo({w - radius, 0.0F})
        .CubicTo({w - radius * 0.45F, 0.0F}, {w, radius * 0.45F}, {w, radius})
        .LineTo({w, std::max(radius, h - radius)})
        .CubicTo({w, h - radius * 0.45F}, {w - radius * 0.45F, h}, {w - radius, h})
        .LineTo({radius, h})
        .CubicTo({radius * 0.45F, h}, {0.0F, h - radius * 0.45F}, {0.0F, h - radius})
        .LineTo({0.0F, radius})
        .CubicTo({0.0F, radius * 0.45F}, {radius * 0.45F, 0.0F}, {radius, 0.0F})
        .Close();
}

huxerui::View ResizeHandle(huxerui::Axis axis, huxerui::State<float> value,
                           huxerui::State<float> origin, float minimum, float maximum,
                           bool reverse = false) {
    const bool horizontal = axis == huxerui::Axis::Horizontal;
    const huxerui::Axis dragAxis = horizontal ? huxerui::Axis::Vertical
                                              : huxerui::Axis::Horizontal;
    huxerui::Frame frame;
    if (horizontal)
        frame.height = 8.0F;
    else
        frame.width = 8.0F;
    return huxerui::Stack {huxerui::Divider(axis)}
        .With(frame,
              huxerui::PointerCursor(horizontal ? huxerui::PointerCursorKind::ResizeVertical
                                                 : huxerui::PointerCursorKind::ResizeHorizontal),
              huxerui::DragGesture{.axis = dragAxis, .minimum_distance = 0.0F})
        .On<huxerui::DragEvents::Started>([origin, value](const huxerui::DragEvent&) {
            origin = value.Get();
        })
        .On<huxerui::DragEvents::Changed>([origin, value, minimum, maximum, reverse, horizontal](const huxerui::DragEvent& event) {
            const float delta = horizontal ? event.translation.y : event.translation.x;
            const float signedDelta = reverse ? -delta : delta;
            value = std::clamp(origin.Get() + signedDelta, minimum, maximum);
        });
}

huxerui::View GoogleRequestSurface(huxerui::View content, std::size_t selectedIndex,
                                   const huxerui::ThemeSpec& theme) {
    const huxerui::Color surface = theme.colors.surface_container_low;
    const huxerui::Color border = theme.colors.outline;
    return huxerui::Stack {
        huxerui::Canvas([surface, border, selectedIndex](huxerui::PaintContext& paint,
                                                           huxerui::Size size) {
            const huxerui::Path outline = MakeConnectedTabOutline(size, selectedIndex);
            paint.FillPath(outline, surface);
            paint.StrokePath(outline, border, huxerui::StrokeStyle{.width = 1.0F});
        }),
        std::move(content),
    }.With(huxerui::Grow(1.0F), huxerui::ClipChildren());
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
// 可重选文件重试。导入执行为纯同步 store 调用，整体推迟出指针事件路径
// （tasks.Launch + Delay(0)，CLAUDE.md 约定 6）。
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

    // 解析一段文件全文：成功入 parsed，失败入 importError（互相清零）。
    auto parseText = [parsed, importError](const std::string& text) {
        auto api = std::make_shared<ImportedApi>();
        std::string err;
        if (!ParseApiFile(text, *api, err)) {
            parsed = nullptr;
            importError = err;
            return;
        }
        importError = std::string{};
        parsed = api;
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
            huxerui::Button("解析粘贴内容").OnClick([pasteValue, parseText] {
                parseText(pasteValue.Get().text);
            }),
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
        huxerui::Button("导入").OnClick([ctx, tasks, toast, listVersion, parsed] {
            const auto api = parsed.Get();
            if (api == nullptr) {
                toast.Show("请先选择文件并成功解析");
                return;
            }
            ctx.Dismiss();
            // 导入重组左岛列表：推迟出指针事件路径（约定 6）。dirChain 逐级
            // find-or-create 分组（Name 模式，仅组织作用、不参与 URL），每条
            // op 组装 db::SavedRequest 落库；遇错即停并 toast。
            tasks.Launch([api, toast, listVersion]() -> huxerui::Task<void> {
                co_await huxerui::Delay(std::chrono::duration<double>{0});
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
                            if (const std::string err = g_requests.createGroup(
                                    seg, db::GroupMode::Name, parent);
                                !err.empty()) {
                                toast.Show("导入失败: " + err);
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
                    if (failed) co_return;
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
                    if (const std::string err = g_requests.save(rec); !err.empty()) {
                        toast.Show("导入失败: " + err);
                        co_return;
                    }
                    ++count;
                }
                toast.Show(std::format("已导入 {} 个接口", count));
                listVersion = listVersion.Get() + 1;
            });
        }),
    }
                      .With(huxerui::MainAlign(huxerui::MainAxisAlignment::SpaceBetween)));
    return DialogCard(huxerui::Column(std::move(children))
                          .With(huxerui::Spacing(12.0F), huxerui::Frame{.width = 520.0F},
                                huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch)));
}

[[huxerui::composable]] huxerui::View RequestPage(huxerui::State<std::int64_t> opened) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();

    // 未打开项目的兜底已删：主页整宽覆盖侧栏后本页在 opened==0 时不可达。
    // 仍订阅 opened：切换项目时，保活的请求页需要重组一次，让环境选择器从
    // 当前项目游标重新读取并显示对应环境。
    (void)opened.Get();

    // 内部标签页：每个打开的请求一个草稿；响应区状态为页面级（单引擎）。
    auto openDrafts = huxerui::UseState<std::vector<RequestDraft>>({});
    auto activeTab = huxerui::UseState<std::size_t>(0);
    auto newTabOpen = huxerui::UseState(false);
    auto listVersion = huxerui::UseState(0);
    // 环境版本：环境选择/CRUD/弹窗保存后 bump，标签条与环境弹窗按它重读 store。
    auto envVersion = huxerui::UseState(0);
    // 编辑器子页：0=调试 1=文档 2=测试用例 3=Mock（仅 HTTP 草稿用；非调试页时
    // 响应下岛让位，编辑器占满右岛）。
    auto editorPage = huxerui::UseState<std::size_t>(0);
    auto inFlight = huxerui::UseState(false);
    auto responseBody = huxerui::UseState(std::string{"（尚未发送请求）"});
    auto responseHeaders = huxerui::UseState<std::vector<std::string>>({});
    auto responseCookies = huxerui::UseState<std::vector<std::string>>({});
    auto leftIslandWidth = huxerui::UseState(260.0F);
    auto leftIslandOrigin = huxerui::UseState(260.0F);
    auto responseIslandHeight = huxerui::UseState(260.0F);
    auto responseIslandOrigin = huxerui::UseState(260.0F);

    // 响应式：Compact（<600pt）改上下堆叠，Medium/Expanded 保持左右双岛。
    const bool compact = huxerui::UseViewportClass() == huxerui::ViewportClass::Compact;

    const std::vector<RequestDraft> snapshot = openDrafts.Get();
    const std::size_t current =
        snapshot.empty() ? 0 : std::min(activeTab.Get(), snapshot.size() - 1);
    const int currentKind = snapshot.empty() ? 0 : snapshot[current].kind;
    // 右岛内容 Key：kind + 标签下标组合，切标签/切类型时正确重组。
    const std::int64_t contentKey = static_cast<std::int64_t>(current) * 4 + currentKind;

    // 右侧按活跃草稿类型分派：
    // - HTTP：拆上下两岛——上岛 = 标签条 + 编辑器（Grow 3），下岛 = 响应区
    //   （Grow 2）；编辑器固定部分（名称/操作栏/分区切换/Body 类型行）不滚动，
    //   分区内容与响应各自内滚（垂直滚动条）。
    // - WS/TCP：直接嵌入整页组件（内部自带 ScrollView/事件泵，勿再套 ScrollView，
    //   避免同轴嵌套滚动）；固定 kUid=1 引擎会话，同类型标签共享同一条连接。
    // - gRPC（kind=3）：引擎未实现——整页占位（仅标签条 + 提示文案，无操作栏、
    //   无分区、不可保存/发送；gRPC 草稿不落库）。
    // - 无打开草稿：单岛空状态。
    huxerui::View rightArea = huxerui::Column{};
    if (snapshot.empty() || newTabOpen.Get()) {
        auto createDraft = [openDrafts, activeTab, newTabOpen](int kind) {
            std::vector<RequestDraft> copy = openDrafts.Get();
            copy.push_back(RequestDraft{.kind = kind});
            openDrafts = copy;
            activeTab = copy.size() - 1;
            newTabOpen = false;
        };
        rightArea = GoogleRequestSurface(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
                NewRequestChooser(createDraft),
            }
                .With(huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                      huxerui::MainAlign(huxerui::MainAxisAlignment::Center),
                      huxerui::Padding(theme.spacing.medium),
                      huxerui::Grow(1.0F)),
            current, theme);
    } else if (currentKind == 1 || currentKind == 2) {
        huxerui::View content = currentKind == 1
                                    ? WebSocketPage().With(huxerui::Grow(1.0F)).Key(contentKey)
                                    : TcpPage().With(huxerui::Grow(1.0F)).Key(contentKey);
        rightArea = GoogleRequestSurface(
            huxerui::Column {
                RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
                std::move(content),
            }
                .With(huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Padding(theme.spacing.medium),
                      huxerui::Grow(1.0F)),
            current, theme);
    } else if (currentKind == 3) {
        // gRPC 占位页：支持规划中——只给标签条 + 居中提示，无操作栏/分区。
        rightArea = GoogleRequestSurface(
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
                .With(huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                      huxerui::Padding(theme.spacing.medium),
                      huxerui::Grow(1.0F)),
            current, theme);
    } else {
        // 非调试子页（文档/测试用例/Mock）：编辑器占满右岛，响应下岛让位。
        const bool debugging = editorPage.Get() == 0;
        std::vector<huxerui::View> islands;
        huxerui::View editorIsland = huxerui::Column {
            RequestTabStrip(openDrafts, activeTab, envVersion, newTabOpen),
            RequestEditor(openDrafts, current, activeTab, listVersion,
                          inFlight, responseBody, responseHeaders,
                          responseCookies, envVersion, editorPage)
                .Key(contentKey)
                .With(huxerui::Grow(1.0F)),
        }
            .With(huxerui::Spacing(theme.spacing.medium),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                  huxerui::Padding(theme.spacing.medium),
                  huxerui::Grow(debugging ? 3.0F : 1.0F));
        islands.push_back(GoogleRequestSurface(std::move(editorIsland), current,
                                                theme));
        if (debugging) {
            islands.push_back(ResizeHandle(huxerui::Axis::Horizontal, responseIslandHeight,
                                           responseIslandOrigin, 140.0F, 640.0F,
                                           true));
            islands.push_back(huxerui::Column {
                    ResponseArea(responseBody, responseHeaders, responseCookies, inFlight, theme)
                        .With(huxerui::Grow(1.0F)),
                }
                    .With(huxerui::Spacing(theme.spacing.small),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                          huxerui::Padding(theme.spacing.medium),
                          huxerui::Frame{.height = responseIslandHeight.Get()}));
        }
        rightArea = huxerui::Column(std::move(islands))
                        .With(huxerui::Spacing(theme.spacing.small),
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
                              huxerui::Grow(1.0F));
    }

    if (compact) {
        // Compact：上 = 请求列表（限高 220、宽度撑满），下 = 编辑区（撑满剩余）。
        return huxerui::Column {
                   RequestListIsland(openDrafts, activeTab, listVersion, true, 260.0F),
                   std::move(rightArea),
               }
            .With(huxerui::Spacing(theme.spacing.small),
                  huxerui::Grow(1.0F),
                  huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
    }
    return huxerui::Row {
               RequestListIsland(openDrafts, activeTab, listVersion, false,
                                 leftIslandWidth.Get()),
               ResizeHandle(huxerui::Axis::Vertical, leftIslandWidth, leftIslandOrigin,
                            180.0F, 420.0F),
               std::move(rightArea),
           }
        .With(huxerui::Spacing(theme.spacing.small),
              huxerui::Grow(1.0F),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch));
}

} // namespace apitab::ui
