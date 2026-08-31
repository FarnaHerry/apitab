// testcase_page.cpp — 请求编辑器子页"测试用例"（pageTab=2）：用例编辑 + 运行用例。
// 编辑一律经 MutateDraft 写回共享草稿 cases（持久化仍由调试页"保存"全量落库，
// 见 request_page.cpp）；运行照抄调试页发送流程（SpecFromDraft → finalizeSpec →
// sendViaEngine → UI 协程 PollWhile 轮询 takeResponse），响应回来后在 UI 线程对
// 启用用例逐条求值（状态码 / 耗时上限 / JSON 路径断言），测试运行不落历史。
#include <huxerui/huxerui.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "ui.h"
#include "draft.h"
#include "task_bridge.h"

import apitab.api_engine;
import apitab.store.requests;
import apitab.utils;
import nlohmann.json;

namespace apitab::ui {

namespace {

// ---- 草稿 → 请求规格 ----
// SpecFromDraft / ToKeyValue 在 request_page.cpp 的匿名 ns（内部链接），跨 TU
// 不可复用，此处按同一逻辑复制一份静态版本。

api::KeyValue ToKeyValue(const KvRow& row) {
    return api::KeyValue{.key = row.key.text,
                         .value = row.value.text,
                         .enabled = row.enabled,
                         .type = row.type.text,
                         .remark = row.remark.text};
}

api::RequestSpec SpecFromDraft(const RequestDraft& draft) {
    api::RequestSpec spec;
    spec.method = std::string{kMethodNames.at(draft.methodIndex)};
    spec.url = draft.url.text;
    for (const KvRow& row : draft.params)
        if (row.enabled && !row.key.text.empty()) spec.params.push_back(ToKeyValue(row));
    for (const KvRow& row : draft.headers)
        if (row.enabled && !row.key.text.empty()) spec.headers.push_back(ToKeyValue(row));
    for (const KvRow& row : draft.cookies)
        if (row.enabled && !row.key.text.empty()) spec.cookies.push_back(ToKeyValue(row));
    spec.bodyKind = static_cast<api::BodyKind>(draft.bodyKindIndex);
    switch (spec.bodyKind) {
        case api::BodyKind::None:
            break;
        case api::BodyKind::FormUrlEncoded:
        case api::BodyKind::FormData:
            for (const KvRow& row : draft.bodyFields)
                if (row.enabled && !row.key.text.empty())
                    spec.bodyFields.push_back(ToKeyValue(row));
            break;
        default: // Json/Text/Xml/GraphQL：文本体
            spec.body = draft.bodies[draft.bodyKindIndex].text;
            break;
    }
    return spec;
}

// ---- 数字条件解析（草稿侧文本承载；空 = 不校验，非法 = 校验失败）----

bool ParseLongField(const std::string& raw, long& out) {
    const std::string s = trim(raw);
    if (s.empty()) return false;
    out = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

bool ParseDoubleField(const std::string& raw, double& out) {
    const std::string s = trim(raw);
    if (s.empty()) return false;
    out = 0.0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

// ---- JSON 路径求值：点分段 + 下标（如 data.items[0].id）----
// 任一段缺失 / 类型不符 / 语法不完整 → nullptr（断言按"路径不存在"失败）。
const nlohmann::json* ResolveJsonPath(const nlohmann::json& root, std::string_view path) {
    const nlohmann::json* node = &root;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t dot = path.find('.', pos);
        const std::string_view seg =
            path.substr(pos, dot == std::string_view::npos ? std::string_view::npos : dot - pos);
        bool descended = false;
        const std::size_t bracket = seg.find('[');
        if (const std::string_view name = seg.substr(0, bracket); !name.empty()) {
            if (!node->is_object()) return nullptr;
            const auto it = node->find(std::string{name});
            if (it == node->end()) return nullptr;
            node = &*it;
            descended = true;
        }
        // '[' 起的一串下标（支持 a[0][1]）；']' 后只允许再接 '['。
        std::size_t p = bracket == std::string_view::npos ? seg.size() : bracket;
        while (p < seg.size()) {
            if (seg[p] != '[') return nullptr;
            const std::size_t close = seg.find(']', p);
            if (close == std::string_view::npos) return nullptr;
            const std::string_view digits = seg.substr(p + 1, close - p - 1);
            std::size_t index = 0;
            const auto [end, ec] =
                std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (ec != std::errc{} || end != digits.data() + digits.size()) return nullptr;
            if (!node->is_array() || index >= node->size()) return nullptr;
            node = &(*node)[index];
            descended = true;
            p = close + 1;
        }
        if (!descended) return nullptr; // 空段（连续点 / 首尾点等残缺语法）
        if (dot == std::string_view::npos) return node;
        pos = dot + 1;
    }
}

// json 值 → 比较/展示文本：字符串取原文，其余（数字/布尔/null/容器）dump()。
std::string JsonValueText(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    return value.dump();
}

// ---- 运行结果 ----

struct CaseCheck {
    std::string text;    // 校验项描述（含期望值，失败时附实际值）
    bool passed = false; // 通过状态（渲染时加 ✓/✗ 后缀）

    bool operator==(const CaseCheck&) const = default; // State 变更检测需要相等性
};

struct CaseResult {
    bool passed = false;
    // 空 = 本用例未参与本次运行（disabled 或无任何校验项，不参与统计）。
    std::vector<CaseCheck> checks;

    bool operator==(const CaseResult&) const = default;
};

// 用例是否含至少一条生效校验项（数字条件非空，或某条启用且有路径的断言）。
bool CaseHasChecks(const TestCaseDraft& c) {
    if (!trim(c.expectStatus.text).empty() || !trim(c.maxMs.text).empty()) return true;
    for (const KvRow& row : c.asserts)
        if (row.enabled && !trim(row.key.text).empty()) return true;
    return false;
}

// 对响应逐用例求值。results 与 cases 下标对齐；未参与的下标保持空 checks。
std::vector<CaseResult> EvaluateCases(const std::vector<TestCaseDraft>& cases,
                                      const api::ResponseView& view) {
    std::vector<CaseResult> out(cases.size());
    // JSON 只在真正有路径断言时惰性解析一次（非 JSON 响应 = 断言失败）。
    bool json_tried = false;
    bool json_ok = false;
    nlohmann::json root;
    for (std::size_t ci = 0; ci < cases.size(); ++ci) {
        const TestCaseDraft& c = cases[ci];
        if (!c.enabled || !CaseHasChecks(c)) continue;
        CaseResult r;
        const auto check = [&r](std::string text, bool passed) {
            r.checks.push_back(CaseCheck{.text = std::move(text), .passed = passed});
        };
        if (!view.ok) { // 传输层失败：本用例整体失败，一条原因即够
            check("请求失败: " + view.error, false);
            out[ci] = std::move(r);
            continue;
        }
        bool all = true;
        const auto expect = [&check, &all](std::string text, bool passed) {
            all = all && passed;
            check(std::move(text), passed);
        };
        if (!trim(c.expectStatus.text).empty()) {
            long want = 0;
            if (ParseLongField(c.expectStatus.text, want)) {
                expect("状态码 " + std::to_string(want) +
                           (view.status == want ? ""
                                                : "（实际 " + std::to_string(view.status) + "）"),
                       view.status == want);
            } else {
                expect("状态码条件无法解析: " + trim(c.expectStatus.text), false);
            }
        }
        if (!trim(c.maxMs.text).empty()) {
            double limit = 0.0;
            if (ParseDoubleField(c.maxMs.text, limit)) {
                expect(std::format("耗时 {:.0f}ms ≤ {:.0f}ms", view.totalMs, limit),
                       view.totalMs <= limit);
            } else {
                expect("耗时上限无法解析: " + trim(c.maxMs.text), false);
            }
        }
        for (const KvRow& row : c.asserts) {
            if (!row.enabled || trim(row.key.text).empty()) continue;
            const std::string path = trim(row.key.text);
            if (!json_tried) {
                json_tried = true;
                root = nlohmann::json::parse(view.body, nullptr, false);
                json_ok = !root.is_discarded();
            }
            if (!json_ok) {
                expect("断言 " + path + "（响应体不是有效 JSON）", false);
                continue;
            }
            const nlohmann::json* node = ResolveJsonPath(root, path);
            if (node == nullptr) {
                expect(path + " 路径不存在", false);
                continue;
            }
            const std::string actual = JsonValueText(*node);
            const bool passed = actual == row.value.text;
            expect(path + " = " + row.value.text + (passed ? "" : "（实际 " + actual + "）"),
                   passed);
        }
        r.passed = all;
        out[ci] = std::move(r);
    }
    return out;
}

} // namespace

[[huxerui::composable]] huxerui::View TestCasePage(RequestDraft snapshot,
                                                   huxerui::State<std::vector<RequestDraft>> drafts,
                                                   std::size_t index) {
    const huxerui::ThemeSpec& theme = huxerui::UseTheme();
    auto tasks = huxerui::UseTaskScope();
    auto toast = huxerui::UseToast();
    auto running = huxerui::UseState<bool>(false); // 运行在途（禁用按钮、变文案）
    auto results = huxerui::UseState<std::vector<CaseResult>>(std::vector<CaseResult>{});
    // 用例结构代际：在途运行期间删卡会让下标错位，代际不符则结果作废。
    auto runGen = huxerui::UseState<std::uint64_t>(0);

    const std::vector<TestCaseDraft> cases = snapshot.cases;
    const std::vector<CaseResult> res = results.Get(); // 订阅：运行结束/清结果即重组

    const bool lightTheme = theme.colors.surface.red > 0.5F;
    const huxerui::Color passColor = lightTheme ? huxerui::Color::Rgb(46, 125, 50)
                                                : huxerui::Color::Rgb(107, 203, 119);
    const huxerui::Color failColor = theme.colors.error;

    // ---- 运行流程（照抄 request_page 发送：推迟出指针事件路径 + PollWhile）----
    const auto runCases = [tasks, drafts, index, toast, running, results, runGen] {
        tasks.Launch([=]() -> huxerui::Task<void> {
            // 翻转运行中状态会重组按钮子树：State 写入整体推迟出指针事件路径
            // （CLAUDE.md 约定 6）。
            co_await huxerui::Delay(std::chrono::duration<double>{0});
            if (running.Get()) co_return;
            const std::vector<RequestDraft> current = drafts.Get();
            if (index >= current.size()) co_return;
            const RequestDraft draft = current[index]; // 冻结本次运行的用例集合
            bool anyRunnable = false;
            for (const TestCaseDraft& c : draft.cases)
                if (c.enabled && CaseHasChecks(c)) { anyRunnable = true; break; }
            if (!anyRunnable) {
                toast.Show("没有可运行的用例（启用用例并至少填一条校验）");
                co_return;
            }
            api::RequestSpec spec = SpecFromDraft(draft);
            if (spec.url.empty()) {
                toast.Show("URL 不能为空");
                co_return;
            }
            const api::RequestSpec finalSpec = g_requests.finalizeSpec(spec);
            running = true;
            results = std::vector<CaseResult>{}; // 旧结果作废（用例可能已编辑）
            runGen = runGen.Get() + 1;
            const std::uint64_t gen = runGen.Get();
            api::ResponseView stale; // 丢弃引擎里的残留结果，只轮询本次运行的
            while (g_requests.takeResponse(stale)) {}
            g_requests.sendViaEngine(finalSpec);
            tasks.Launch([=]() -> huxerui::Task<void> {
                // 引擎结果按 30ms 节拍轮询取回；恢复点恒为 UI 线程（task_bridge.h）。
                // 测试运行是内部动作，不 recordHistory。
                api::ResponseView view;
                co_await PollWhile(std::chrono::duration<double>{0.03},
                                   [&view] { return !g_requests.takeResponse(view); });
                // 在途期间用例被删改（代际变化）→ 下标已错位，结果作废。
                if (gen == runGen.Get()) results = EvaluateCases(draft.cases, view);
                running = false;
            });
        });
    };

    // ---- 草稿写回帮助：只动本用例的下标字段，越界静默忽略 ----
    const auto setCaseEnabled = [drafts, index](std::size_t ci, bool enabled) {
        MutateDraft(drafts, index,
                    [ci, enabled](RequestDraft& d) {
                        if (ci < d.cases.size()) d.cases[ci].enabled = enabled;
                    });
    };
    const auto setCaseName = [drafts, index](std::size_t ci, huxerui::TextEditingValue name) {
        MutateDraft(drafts, index,
                    [ci, name = std::move(name)](RequestDraft& d) {
                        if (ci < d.cases.size()) d.cases[ci].name = name;
                    });
    };
    const auto setCaseAsserts = [drafts, index](std::size_t ci, std::vector<KvRow> rows) {
        MutateDraft(drafts, index,
                    [ci, rows = std::move(rows)](RequestDraft& d) {
                        if (ci < d.cases.size()) d.cases[ci].asserts = std::move(rows);
                    });
    };
    const auto addCase = [drafts, index] {
        MutateDraft(drafts, index, [](RequestDraft& d) { d.cases.emplace_back(); });
    };
    const huxerui::View addButton = huxerui::Button("+ 添加用例").OnClick(addCase);

    std::vector<huxerui::View> pageChildren;

    // 顶栏：运行按钮 + 本次汇总（无用例/未参与统计时只有按钮）。
    int runCount = 0, passCount = 0;
    for (std::size_t i = 0; i < cases.size() && i < res.size(); ++i) {
        if (res[i].checks.empty()) continue;
        ++runCount;
        if (res[i].passed) ++passCount;
    }
    std::vector<huxerui::View> runRow{
        huxerui::Button(running.Get() ? "运行中…" : "运行用例")
            .With(huxerui::Enabled(!running.Get()))
            .OnClick(runCases),
    };
    if (runCount > 0)
        runRow.push_back(huxerui::Text(std::format("通过 {}/{}", passCount, runCount),
                                       huxerui::TextRole::Label)
                             .With(huxerui::Foreground(
                                 passCount == runCount ? passColor : failColor)));
    pageChildren.push_back(huxerui::Row(std::move(runRow))
                               .With(huxerui::Spacing(theme.spacing.small),
                                     huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));

    if (cases.empty()) {
        pageChildren.push_back(huxerui::Column{
            huxerui::Text("还没有测试用例。为响应添加状态码 / 耗时上限 / JSON 路径断言，"
                          "点「运行用例」一次性校验。",
                          huxerui::TextRole::Label)
                .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            addButton,
        }
                                   .With(huxerui::Spacing(theme.spacing.small),
                                         huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center),
                                         huxerui::Grow(1.0F)));
    } else {
        std::vector<huxerui::View> cards;
        for (std::size_t ci = 0; ci < cases.size(); ++ci) {
            const TestCaseDraft c = cases[ci];
            const bool evaluated = ci < res.size() && !res[ci].checks.empty();

            // 行头：启用 + 用例名 + PASS/FAIL 汇总徽标 + 删除。
            huxerui::View badge = huxerui::View{huxerui::Text("", huxerui::TextRole::Label)};
            if (evaluated)
                badge = huxerui::View{
                    huxerui::Text(res[ci].passed ? "PASS" : "FAIL", huxerui::TextRole::Label)
                        .Style(huxerui::TextStyle{
                            .font = huxerui::Font::Monospace(font_size::kChip)
                                        .WithWeight(huxerui::FontWeight::SemiBold),
                            .foreground = res[ci].passed ? passColor : failColor})};
            std::vector<huxerui::View> header{
                huxerui::Checkbox(c.enabled).OnChanged([setCaseEnabled, ci](bool checked) {
                    setCaseEnabled(ci, checked);
                }),
                huxerui::TextField(c.name)
                    .Label("用例名")
                    .Placeholder("用例名称")
                    .Variant(huxerui::TextFieldVariant::Standard)
                    .OnChanged([setCaseName, ci](const huxerui::TextEditingValue& value) {
                        setCaseName(ci, value);
                    })
                    .With(huxerui::Grow(1.0F)),
                badge,
                huxerui::Button("✕").OnClick([tasks, drafts, index, ci, results, runGen] {
                    // 删除会卸载本按钮所在卡片：写回推迟出指针事件路径（约定 6）；
                    // 结果向量与用例按下标对齐，删一行会整体错位 → 清空结果并升代际
                    // （作废在途运行的回写）。
                    tasks.Launch([=]() -> huxerui::Task<void> {
                        co_await huxerui::Delay(std::chrono::duration<double>{0});
                        MutateDraft(drafts, index, [ci](RequestDraft& d) {
                            if (ci < d.cases.size())
                                d.cases.erase(d.cases.begin() + static_cast<long>(ci));
                        });
                        results = std::vector<CaseResult>{};
                        runGen = runGen.Get() + 1;
                    });
                }),
            };

            std::vector<huxerui::View> card{
                huxerui::Row(std::move(header))
                    .With(huxerui::Spacing(theme.spacing.small),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                // 数字条件行：空 = 不校验（受控 TextField 保留完整 TextEditingValue）。
                huxerui::Row {
                    huxerui::TextField(c.expectStatus)
                        .Label("期望状态码")
                        .Placeholder("200（空=不校验）")
                        .Variant(huxerui::TextFieldVariant::Standard)
                        .OnChanged([drafts, index, ci](const huxerui::TextEditingValue& value) {
                            MutateDraft(drafts, index, [ci, value](RequestDraft& d) {
                                if (ci < d.cases.size()) d.cases[ci].expectStatus = value;
                            });
                        })
                        .With(huxerui::Grow(1.0F)),
                    huxerui::TextField(c.maxMs)
                        .Label("耗时上限 ms")
                        .Placeholder("500（空=不校验）")
                        .Variant(huxerui::TextFieldVariant::Standard)
                        .OnChanged([drafts, index, ci](const huxerui::TextEditingValue& value) {
                            MutateDraft(drafts, index, [ci, value](RequestDraft& d) {
                                if (ci < d.cases.size()) d.cases[ci].maxMs = value;
                            });
                        })
                        .With(huxerui::Grow(1.0F)),
                }
                    .With(huxerui::Spacing(theme.spacing.small),
                          huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)),
                huxerui::Text("JSON 断言", huxerui::TextRole::Label)
                    .With(huxerui::Foreground(theme.colors.on_surface_variant)),
            };

            // 断言子表：虚拟空行物化 + ✕ 仅真实行（照抄 KvTable 语义）。
            card.push_back(huxerui::Row {
                huxerui::Text("", huxerui::TextRole::Label).With(huxerui::Frame{.width = 24.0F}),
                huxerui::Text("JSON 路径", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
                huxerui::Text("期望值", huxerui::TextRole::Label).With(huxerui::Grow(1.0F)),
                huxerui::Text("", huxerui::TextRole::Label),
            }
                               .With(huxerui::Spacing(theme.spacing.small),
                                     huxerui::Foreground(theme.colors.on_surface_variant)));
            for (std::size_t i = 0; i <= c.asserts.size(); ++i) {
                const bool phantom = i == c.asserts.size();
                const KvRow row = phantom ? KvRow{} : c.asserts[i];
                // 行写入：i 越界（虚拟行）时物化新行，否则改写原行；虚拟行仅在
                // 键/值真的输入了文本时才物化（纯聚焦/移动光标的 OnChanged 不追加）。
                auto applyRow = [rows = c.asserts, ci, setCaseAsserts](std::size_t i,
                                                                       KvRow updated) {
                    std::vector<KvRow> copy = rows;
                    if (i < copy.size()) {
                        copy[i] = std::move(updated);
                    } else {
                        if (updated.key.text.empty() && updated.value.text.empty()) return;
                        copy.push_back(std::move(updated));
                    }
                    setCaseAsserts(ci, std::move(copy));
                };
                card.push_back(
                    huxerui::Row {
                        huxerui::Checkbox(row.enabled)
                            .OnChanged([row, i, applyRow](bool checked) {
                                KvRow updated = row;
                                updated.enabled = checked;
                                applyRow(i, std::move(updated));
                            }),
                        huxerui::TextField(row.key)
                            .Placeholder("data.items[0].id")
                            .Variant(huxerui::TextFieldVariant::Standard)
                            .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                                KvRow updated = row;
                                updated.key = value;
                                applyRow(i, std::move(updated));
                            })
                            .With(huxerui::Grow(1.0F)),
                        huxerui::TextField(row.value)
                            .Placeholder("期望值")
                            .Variant(huxerui::TextFieldVariant::Standard)
                            .OnChanged([row, i, applyRow](const huxerui::TextEditingValue& value) {
                                KvRow updated = row;
                                updated.value = value;
                                applyRow(i, std::move(updated));
                            })
                            .With(huxerui::Grow(1.0F)),
                        phantom
                            ? huxerui::View{huxerui::Text("", huxerui::TextRole::Label)
                                                .With(huxerui::Padding(4.0F))}
                            : huxerui::View{huxerui::Button("✕").OnClick(
                                  [tasks, rows = c.asserts, i, ci, setCaseAsserts] {
                                      // 删除会移除本按钮所在行：推迟出指针事件路径。
                                      tasks.Launch([=]() -> huxerui::Task<void> {
                                          co_await huxerui::Delay(
                                              std::chrono::duration<double>{0});
                                          std::vector<KvRow> copy = rows;
                                          if (i < copy.size())
                                              copy.erase(copy.begin() + static_cast<long>(i));
                                          setCaseAsserts(ci, std::move(copy));
                                      });
                                  })},
                    }
                        .With(huxerui::Spacing(theme.spacing.small),
                              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Center)));
            }

            // 运行结果：逐条校验行（✓ 绿 / ✗ 红），列在断言子表下方。
            if (evaluated) {
                for (const CaseCheck& chk : res[ci].checks) {
                    card.push_back(huxerui::Text(
                                       chk.text + (chk.passed ? " ✓" : " ✗"),
                                       huxerui::TextRole::Label)
                                       .With(huxerui::Foreground(
                                           chk.passed ? passColor : failColor)));
                }
            }

            cards.push_back(huxerui::Column(std::move(card))
                                .With(huxerui::Padding(theme.spacing.medium),
                                      huxerui::Spacing(theme.spacing.small),
                                      huxerui::Background(theme.colors.surface_container),
                                      huxerui::CornerRadius(theme.shapes.medium),
                                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))
                                .Key(ci));
        }
        cards.push_back(addButton);
        pageChildren.push_back(huxerui::ScrollView{
            huxerui::Column(std::move(cards))
                .With(huxerui::Spacing(theme.spacing.medium),
                      huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch))}
                                   .With(huxerui::ScrollBar(), huxerui::Grow(1.0F)));
    }

    pageChildren.push_back(
        huxerui::Text("改动需回「调试」页点「保存」落库", huxerui::TextRole::Label)
            .With(huxerui::Foreground(theme.colors.on_surface_variant)));

    return huxerui::Column(std::move(pageChildren))
        .With(huxerui::Spacing(theme.spacing.medium),
              huxerui::CrossAlign(huxerui::CrossAxisAlignment::Stretch),
              huxerui::Grow(1.0F));
}

} // namespace apitab::ui
