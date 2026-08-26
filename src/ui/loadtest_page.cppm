// ui/loadtest_page.cppm — 压测页：k6 引擎状态 + 参数 + 启停 + 实时输出 + 结果汇总/历史记录。
module;

#include "eui_ui.h"

export module apitab.ui.loadtest_page;

import std;
import apitab.api_engine;
import apitab.db;
import apitab.store.loadtest;  // g_loadtest
import apitab.store.requests;  // g_requests（草稿 → spec）
import apitab.store.ui;
import apitab.ui.theme;
import apitab.ui.utils;
import apitab.ui.widgets;

namespace {

float g_outputScroll = 0.0f;

} // namespace

// 历史压测记录缓存（进入页面 / 新记录落库后置 dirty 刷新，不每帧查库）。
std::vector<db::LoadRecord> g_records;
bool g_recordsDirty = true;

// app.cpp 在新汇总落库后调用（记录页下次绘制时重查库）。
export void markLoadRecordsDirty() { g_recordsDirty = true; }

namespace {

void startLoad() {
    RequestTab& tab = activeTab();
    if (tab.draft.kind != api::RequestKind::Http) {
        showStatus("只有 HTTP 请求支持 k6 压测");
        return;
    }
    // 与请求页同一套拼接：环境 baseUrl + Path 分组前缀 + 相对路径。
    api::RequestSpec spec = buildSpec(tab.draft, g_requests.composeUrl(
        tab.draft.url, tab.draft.groupId, g_requests.currentEnvId()));
    if (spec.url.empty()) {
        showStatus("URL 不能为空");
        return;
    }
    if (!hasUriScheme(spec.url)) {
        spec.url = "http://" + spec.url;
    }
    // 与单次发送同一套 query 拼接（压测脚本里写死最终 URL）。
    std::vector<std::pair<std::string, std::string>> enabled;
    for (const auto& p : spec.params) {
        if (p.enabled && !p.key.empty()) enabled.emplace_back(p.key, p.value);
    }
    spec.url = appendQuery(spec.url, enabled);

    api::LoadOptions opts;
    try {
        opts.vus = std::max(1, std::stoi(trim(g_vusText)));
    } catch (...) {
        showStatus("VUs 必须是正整数");
        return;
    }
    opts.duration = trim(g_durationText).empty() ? "30s" : trim(g_durationText);

    g_loadOutput.clear();
    g_hasLoadSummary = false;
    g_loadtest.start(spec, opts, tab.requestId, tab.draft.name);
    showStatus(std::format("压测启动: {} VUs / {}", opts.vus, opts.duration));
}

} // namespace

export void drawLoadPage(eui::Ui& ui, float x, float y, float w, float h,
                         const AppTheme& theme) {
    const auto& tokens = theme.components;

    // ---- k6 引擎状态行 ----
    const bool available = g_loadtest.available();
    // 单行最小跨度 ≈ 372（target 文本起点）；低于阈值参数行/按钮行分开，
    // 不再让固定 x+272/x+372 把控件推出岛屿。
    const bool twoRows = w < 372.0f;
    const float controlsH = twoRows ? 90.0f : 58.0f;
    drawIslandPanel(ui, "load.controls.island", x, y, w, controlsH, theme,
                    theme.dark ? 0.56f : 0.78f);
    ui.text("load.engine")
        .position(x + kPanelPad, y + 6.0f)
        .size(nonNegative(w - kPanelPad * 2.0f), 18.0f)
        .text(available ? "k6 引擎: " + g_loadtest.binaryPath()
                        : "未找到 k6 二进制（engines/ 或 PATH），压测不可用")
        .fontSize(kFontLabel)
        .lineHeight(18.0f)
        .color(available ? theme.metaText : theme.serverErr)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // ---- 参数行：VUs / Duration / 启停 ----
    const float cfgY = y + 28.0f;
    const float innerX = x + kPanelPad;
    const float innerW = nonNegative(w - kPanelPad * 2.0f);
    const float btnRowY = twoRows ? cfgY + kInputHeight + kGap : cfgY;
    drawSectionLabel(ui, "load.vus.label", innerX, cfgY + 5.0f, 34.0f, "VUs", theme);
    components::input(ui, "load.vus")
        .position(innerX + 38.0f, cfgY)
        .size(64.0f, kInputHeight)
        .value(g_vusText)
        .placeholder("10")
        .theme(tokens)
        .onChange([](const std::string& v) { g_vusText = v; })
        .build();
    drawSectionLabel(ui, "load.dur.label", innerX + 110.0f, cfgY + 5.0f, 40.0f, "时长", theme);
    components::input(ui, "load.dur")
        .position(innerX + 154.0f, cfgY)
        .size(80.0f, kInputHeight)
        .value(g_durationText)
        .placeholder("30s / 1m")
        .theme(tokens)
        .onChange([](const std::string& v) { g_durationText = v; })
        .build();

    const bool supported = activeTab().draft.kind == api::RequestKind::Http;
    const bool running = g_loadtest.running();
    const float toggleX = twoRows ? innerX : innerX + 246.0f;
    const float targetX = twoRows ? innerX + 96.0f : innerX + 342.0f;
    const float targetW = nonNegative(innerX + innerW - targetX);
    if (!twoRows || innerW >= 100.0f) {
        components::button(ui, "load.toggle")
            .position(toggleX, btnRowY)
            .size(90.0f, kButtonHeight)
            .icon(running ? 0xF04D : 0xF04B)  // stop / play
            .text(running ? "停止" : "开始压测")
            .fontSize(kFontBody)
            .theme(tokens, true)
            .textColor(onPrimaryColor(theme))
            .iconColor(onPrimaryColor(theme))
            .radius(kButtonRadius)
            .disabled((!available || !supported) && !running)
            .onClick([running] {
                if (running) {
                    g_loadtest.stop();
                    showStatus("停止中…");
                } else {
                    startLoad();
                }
            })
            .build();
    }
    if (targetW > 24.0f) {
        ui.text("load.target")
            .position(targetX, btnRowY)
            .size(targetW, kInputHeight)
            .text(!supported ? "当前请求类型不支持 k6 压测" : "目标: " + [&] {
                const std::string finalUrl = g_requests.composeUrl(
                    activeDraft().url, activeDraft().groupId, g_requests.currentEnvId());
                return finalUrl.empty() ? std::string("(当前标签页 URL 为空)") : finalUrl;
            }())
            .fontSize(kFontLabel)
            .lineHeight(kInputHeight)
            .color(theme.metaText)
            .verticalAlign(core::VerticalAlign::Center)
            .build();
    }

    // ---- 实时输出：高度随可用空间分配，不再 min 60 把结果区推出页面 ----
    const float outY = y + controlsH + kGap;
    const float outH = nonNegative((y + h - outY) * 0.45f);
    drawIslandPanel(ui, "load.output.island", x, outY, w, outH, theme,
                    theme.dark ? 0.68f : 0.86f);
    const float outScrollW = nonNegative(w - kPanelPad * 2.0f);
    const float outScrollH = nonNegative(outH - kPanelPad * 2.0f);
    if (outScrollW > 0.0f && outScrollH > 0.0f) {
        // 运行中跟随尾部（超大 offset 由组件 clamp 到底）；结束后自由滚动。
        components::scrollView(ui, "load.out.scroll")
            .position(x + kPanelPad, outY + kPanelPad)
            .size(outScrollW, outScrollH)
            .offset(running ? 1.0e9f : g_outputScroll)
            .theme(tokens)
            .scrollbarWidth(kScrollbarWidth).scrollbarGap(kScrollbarGap)
            .onChange([](float v) { g_outputScroll = v; })
            .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
                if (g_loadOutput.empty()) {
                    cu.text("load.out.empty")
                        .position(4.0f, 4.0f)
                        .size(nonNegative(contentWidth - 8.0f), 16.0f)
                        .text(running ? "等待 k6 输出…" : "（尚无输出）")
                        .fontSize(kFontLabel)
                        .color(theme.hintText)
                        .build();
                    return;
                }
                // 输出按行拼装成一段等宽文本（行高一致，滚动顺畅）。
                // 换行后的真实高度进 scroll root，不用 viewport 高度冒充内容高。
                std::string all;
                for (const auto& l : g_loadOutput) {
                    all += l;
                    all += '\n';
                }
                const float textW = nonNegative(contentWidth - 8.0f);
                const float textH = std::max(viewportH,
                    measureWrappedTextHeight(all, textW, kFontMono, "monospace"));
                cu.text("load.out.text")
                    .position(4.0f, 4.0f)
                    .size(textW, textH)
                    .text(all)
                    .fontFamily("monospace")
                    .fontSize(kFontMono)
                    .color(theme.bodyText)
                    .wrap(true)
                    .build();
            })
            .build();
    }

    // ---- 结果区：本次汇总 / 历史记录 ----
    const float resY = outY + outH + kGap;
    const float resH = nonNegative(y + h - resY);

    drawIslandPanel(ui, "load.results.island", x, resY, w, resH, theme,
                    theme.dark ? 0.56f : 0.78f);
    if (resH < 30.0f) return;  // 太矮只留岛面，不画越界控件
    const float tabsW = std::min(170.0f, nonNegative(w - kPanelPad * 2.0f));
    ui.stack("load.res.tabs.wrap")
        .position(x + kPanelPad, resY + 6.0f)
        .size(tabsW, 22.0f)
        .content([&] {
            components::segmented(ui, "load.res.tabs")
                .size(tabsW, 22.0f)
                .items({"本次汇总", "历史记录"})
                .selected(g_showLoadRecords ? 1 : 0)
                .fontSize(kFontLabel)
                .theme(tokens)
                .style(segmentedStyle(theme))
                .onChange([](int i) {
                    g_showLoadRecords = (i == 1);
                    if (g_showLoadRecords) g_recordsDirty = true;
                })
                .build();
        })
        .build();

    const float tblY = resY + 32.0f;
    const float tblH = nonNegative(resH - 32.0f - 6.0f);
    const float tblW = nonNegative(w - kPanelPad * 2.0f);

    if (!g_showLoadRecords) {
        if (!g_hasLoadSummary) {
            ui.text("load.sum.hint")
                .position(x + kPanelPad, tblY)
                .size(tblW, 18.0f)
                .text(running ? "压测进行中…" : "（尚无汇总 —— 跑一次压测）")
                .fontSize(kFontLabel)
                .color(theme.hintText)
                .build();
            return;
        }
        const auto& s = g_loadSummary;
        if (!s.ok) {
            ui.text("load.sum.err")
                .position(x + kPanelPad, tblY)
                .size(tblW, 18.0f)
                .text("压测异常: " + s.error)
                .fontSize(kFontBody)
                .color(theme.serverErr)
                .build();
            return;
        }
        if (tblH <= 0.0f) return;
        // 与记录表同理：汇总表 2 列在窄窗口也按列文本可用宽截断。
        const float sumTextW = nonNegative(
            (tblW - tokens.metrics.spacing.hairline * 2.0f) / 2.0f
            - tokens.metrics.spacing.section * 2.0f);
        const float sumFont = tokens.metrics.typography.label;
        auto fitSum = [&](const std::string& s) { return fitTextToWidth(s, sumTextW, sumFont); };
        ui.stack("load.sum.table.wrap")
            .position(x + kPanelPad, tblY)
            .size(tblW, tblH)
            .content([&] {
                components::dataTable(ui, "load.sum.table")
                    .size(tblW, tblH)
                    .columns({fitSum("指标"), fitSum("值")})
                    .rows({
                        {fitSum("总请求数"), fitSum(std::to_string(s.requests))},
                        {fitSum("RPS"), fitSum(std::format("{:.1f}", s.rps))},
                        {fitSum("平均耗时"), fitSum(formatMs(s.avgMs))},
                        {fitSum("最小 / 最大"), fitSum(formatMs(s.minMs) + " / " + formatMs(s.maxMs))},
                        {fitSum("P50"), fitSum(formatMs(s.p50Ms))},
                        {fitSum("P90"), fitSum(formatMs(s.p90Ms))},
                        {fitSum("P95"), fitSum(formatMs(s.p95Ms))},
                        {fitSum("P99"), fitSum(formatMs(s.p99Ms))},
                        {fitSum("失败率"), fitSum(formatPct(s.failRate))},
                        {fitSum("实际时长"), fitSum(std::format("{:.1f} s", s.durationSec))},
                    })
                    .theme(tokens)
                    .build();
            })
            .build();
    } else {
        if (g_recordsDirty) {
            g_records = g_loadtest.records();
            g_recordsDirty = false;
        }
        // dataTable 等宽列 + 单元格/表头文本都不裁剪：全部按列文本可用宽截断，
        // 避免窄窗口下长内容溢出到下一列（见 widgets.cppm::fitTextToWidth）。
        // 9 列需要每列至少 textInset*2+正文 ≈ 76px；窗口太窄时减到 5 列核心指标，
        // 不再硬塞 9 列让 textInset 吃掉全部文本宽（单元格全空）。
        const bool fullCols = tblW >= 684.0f;
        const int colCount = fullCols ? 9 : 5;
        const float cellTextW = nonNegative(
            (tblW - tokens.metrics.spacing.hairline * 2.0f) / static_cast<float>(colCount)
            - tokens.metrics.spacing.section * 2.0f);
        const float cellFont = tokens.metrics.typography.label;
        auto fit = [&](const std::string& s) { return fitTextToWidth(s, cellTextW, cellFont); };
        std::vector<std::vector<std::string>> rows;
        for (const auto& r : g_records) {
            if (fullCols) {
                rows.push_back({
                    fit(formatTime(r.createdAt)),
                    fit(r.name),
                    fit(std::format("{}×{}", r.vus, r.duration)),
                    fit(std::to_string(r.requests)),
                    fit(std::format("{:.0f}", r.rps)),
                    fit(formatMs(r.p50Ms)),
                    fit(formatMs(r.p95Ms)),
                    fit(formatMs(r.p99Ms)),
                    fit(formatPct(r.failRate)),
                });
            } else {
                rows.push_back({
                    fit(formatTime(r.createdAt)),
                    fit(r.name),
                    fit(std::to_string(r.requests)),
                    fit(std::format("{:.0f}", r.rps)),
                    fit(formatMs(r.p95Ms)),
                });
            }
        }
        if (tblH <= 0.0f) return;
        ui.stack("load.rec.table.wrap")
            .position(x + kPanelPad, tblY)
            .size(tblW, tblH)
            .content([&] {
                components::dataTable(ui, "load.rec.table")
                    .size(tblW, tblH)
                    .columns(fullCols
                        ? std::vector<std::string>{fit("时间"), fit("名称"), fit("并发×时长"),
                                                   fit("请求数"), fit("RPS"), fit("P50"),
                                                   fit("P95"), fit("P99"), fit("失败率")}
                        : std::vector<std::string>{fit("时间"), fit("名称"), fit("请求数"),
                                                   fit("RPS"), fit("P95")})
                    .rows(std::move(rows))
                    .theme(tokens)
                    .build();
            })
            .build();
    }
}
