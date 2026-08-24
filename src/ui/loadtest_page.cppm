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
    // 与请求页同一套拼接：环境 baseUrl + Path 分组前缀 + 相对路径。
    api::RequestSpec spec = buildSpec(tab.draft, g_requests.composeUrl(
        tab.draft.url, tab.draft.groupId, g_requests.currentEnvId()));
    if (spec.url.empty()) {
        showStatus("URL 不能为空");
        return;
    }
    if (spec.url.find("://") == std::string::npos) {
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
    ui.text("load.engine")
        .position(x, y)
        .size(w, 18.0f)
        .text(available ? "k6 引擎: " + g_loadtest.binaryPath()
                        : "未找到 k6 二进制（engines/ 或 PATH），压测不可用")
        .fontSize(kFontLabel)
        .lineHeight(18.0f)
        .color(available ? theme.metaText : theme.serverErr)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // ---- 参数行：VUs / Duration / 启停 ----
    const float cfgY = y + 24.0f;
    drawSectionLabel(ui, "load.vus.label", x, cfgY + 5.0f, 60.0f, "VUs", theme);
    components::input(ui, "load.vus")
        .position(x + 64.0f, cfgY)
        .size(64.0f, kInputHeight)
        .value(g_vusText)
        .placeholder("10")
        .theme(tokens)
        .onChange([](const std::string& v) { g_vusText = v; })
        .build();
    drawSectionLabel(ui, "load.dur.label", x + 140.0f, cfgY + 5.0f, 60.0f, "时长", theme);
    components::input(ui, "load.dur")
        .position(x + 180.0f, cfgY)
        .size(80.0f, kInputHeight)
        .value(g_durationText)
        .placeholder("30s / 1m")
        .theme(tokens)
        .onChange([](const std::string& v) { g_durationText = v; })
        .build();

    const bool running = g_loadtest.running();
    components::button(ui, "load.toggle")
        .position(x + 272.0f, cfgY)
        .size(90.0f, kButtonHeight)
        .icon(running ? 0xF04D : 0xF04B)  // stop / play
        .text(running ? "停止" : "开始压测")
        .fontSize(kFontBody)
        .theme(tokens, true)
        .textColor(onPrimaryColor(theme))
        .iconColor(onPrimaryColor(theme))
        .disabled(!available && !running)
        .onClick([running] {
            if (running) {
                g_loadtest.stop();
                showStatus("停止中…");
            } else {
                startLoad();
            }
        })
        .build();
    ui.text("load.target")
        .position(x + 372.0f, cfgY)
        .size(std::max(0.0f, w - 372.0f), kInputHeight)
        .text("目标: " + [&] {
            const std::string finalUrl = g_requests.composeUrl(
                activeDraft().url, activeDraft().groupId, g_requests.currentEnvId());
            return finalUrl.empty() ? std::string("(当前标签页 URL 为空)") : finalUrl;
        }())
        .fontSize(kFontLabel)
        .lineHeight(kInputHeight)
        .color(theme.metaText)
        .verticalAlign(core::VerticalAlign::Center)
        .build();

    // ---- 实时输出 ----
    const float outY = cfgY + kInputHeight + kGap;
    const float outH = std::max(60.0f, (y + h - outY) * 0.45f);
    ui.rect("load.out.panel")
        .position(x, outY)
        .size(w, outH)
        .color(components::theme::withAlpha(tokens.surface, theme.dark ? 0.35f : 0.6f))
        .radius(kPanelRadius)
        .build();
    // 运行中跟随尾部（超大 offset 由组件 clamp 到底）；结束后自由滚动。
    components::scrollView(ui, "load.out.scroll")
        .position(x + 4.0f, outY + 4.0f)
        .size(w - 8.0f, outH - 8.0f)
        .offset(running ? 1.0e9f : g_outputScroll)
        .theme(tokens)
        .onChange([](float v) { g_outputScroll = v; })
        .content([&](eui::Ui& cu, float contentWidth, float viewportH) {
            if (g_loadOutput.empty()) {
                cu.text("load.out.empty")
                    .position(4.0f, 4.0f)
                    .size(contentWidth - 8.0f, 16.0f)
                    .text(running ? "等待 k6 输出…" : "（尚无输出）")
                    .fontSize(kFontLabel)
                    .color(theme.hintText)
                    .build();
                return;
            }
            // 输出按行拼装成一段等宽文本（行高一致，滚动顺畅）。
            std::string all;
            for (const auto& l : g_loadOutput) {
                all += l;
                all += '\n';
            }
            cu.text("load.out.text")
                .position(4.0f, 4.0f)
                .size(contentWidth - 8.0f, viewportH)
                .text(all)
                .fontFamily("monospace")
                .fontSize(kFontMono)
                .color(theme.bodyText)
                .wrap(true)
                .build();
        })
        .build();

    // ---- 结果区：本次汇总 / 历史记录 ----
    const float resY = outY + outH + kGap;
    const float resH = y + h - resY;

    ui.stack("load.res.tabs.wrap")
        .position(x, resY)
        .size(170.0f, 22.0f)
        .content([&] {
            components::segmented(ui, "load.res.tabs")
                .size(170.0f, 22.0f)
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

    const float tblY = resY + 26.0f;
    const float tblH = resH - 26.0f;

    if (!g_showLoadRecords) {
        if (!g_hasLoadSummary) {
            ui.text("load.sum.hint")
                .position(x, tblY)
                .size(w, 18.0f)
                .text(running ? "压测进行中…" : "（尚无汇总 —— 跑一次压测）")
                .fontSize(kFontLabel)
                .color(theme.hintText)
                .build();
            return;
        }
        const auto& s = g_loadSummary;
        if (!s.ok) {
            ui.text("load.sum.err")
                .position(x, tblY)
                .size(w, 18.0f)
                .text("压测异常: " + s.error)
                .fontSize(kFontBody)
                .color(theme.serverErr)
                .build();
            return;
        }
        ui.stack("load.sum.table.wrap")
            .position(x, tblY)
            .size(w, tblH)
            .content([&] {
                components::dataTable(ui, "load.sum.table")
                    .size(w, tblH)
                    .columns({"指标", "值"})
                    .rows({
                        {"总请求数", std::to_string(s.requests)},
                        {"RPS", std::format("{:.1f}", s.rps)},
                        {"平均耗时", formatMs(s.avgMs)},
                        {"最小 / 最大", formatMs(s.minMs) + " / " + formatMs(s.maxMs)},
                        {"P50", formatMs(s.p50Ms)},
                        {"P90", formatMs(s.p90Ms)},
                        {"P95", formatMs(s.p95Ms)},
                        {"P99", formatMs(s.p99Ms)},
                        {"失败率", formatPct(s.failRate)},
                        {"实际时长", std::format("{:.1f} s", s.durationSec)},
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
        std::vector<std::vector<std::string>> rows;
        for (const auto& r : g_records) {
            rows.push_back({
                formatTime(r.createdAt),
                r.name,
                std::format("{}×{}", r.vus, r.duration),
                std::to_string(r.requests),
                std::format("{:.0f}", r.rps),
                formatMs(r.p50Ms),
                formatMs(r.p95Ms),
                formatMs(r.p99Ms),
                formatPct(r.failRate),
            });
        }
        ui.stack("load.rec.table.wrap")
            .position(x, tblY)
            .size(w, tblH)
            .content([&] {
                components::dataTable(ui, "load.rec.table")
                    .size(w, tblH)
                    .columns({"时间", "名称", "并发×时长", "请求数", "RPS", "P50", "P95", "P99", "失败率"})
                    .rows(std::move(rows))
                    .theme(tokens)
                    .build();
            })
            .build();
    }
}
