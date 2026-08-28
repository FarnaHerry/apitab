// store/loadtest.cppm — 领域 store：k6 压测的唯一入口（无 eui 依赖）。
// 持有（自保留）k6 引擎与历史记录上下文；结果落库 load_tests。
export module apitab.store.loadtest;

import std;
import apitab.api_engine;
import apitab.config;
import apitab.db;
import apitab.k6_engine;
import apitab.utils;

export class LoadStore {
public:
    LoadStore()
        : engine_(makeK6Engine(cfg::k6Binary().string())),
          db_(std::make_unique<db::Db>(cfg::databaseFile())) {}

    LoadStore(const LoadStore&) = delete;
    LoadStore& operator=(const LoadStore&) = delete;

    void setProject(std::int64_t projectId) { currentProjectId_ = projectId; reloadAutomation(); }
    const std::vector<db::AutomationTest>& automationTests() { reloadAutomation(); return automationTests_; }
    const db::AutomationTest* selectedAutomation() { reloadAutomation(); for (auto& t : automationTests_) if (t.id == selectedAutomationId_) return &t; return nullptr; }
    std::int64_t selectedAutomationId() const { return selectedAutomationId_; }
    void selectAutomation(std::int64_t id) { selectedAutomationId_ = id; }
    std::string saveAutomation(db::AutomationTest& t) { try { t.projectId=currentProjectId_; t.updatedAt=nowUnix(); t.id=db_->saveAutomationTest(t); selectedAutomationId_=t.id; reloadAutomation(); return {}; } catch(const std::exception& e){ return e.what(); } }
    std::string removeAutomation(std::int64_t id) { try { db_->deleteAutomationTest(id,currentProjectId_); if(selectedAutomationId_==id) selectedAutomationId_=0; reloadAutomation(); return {}; } catch(const std::exception& e){ return e.what(); } }
    void reloadAutomation() { if (currentProjectId_ == cachedProjectId_) return; cachedProjectId_=currentProjectId_; try { automationTests_=db_->listAutomationTests(currentProjectId_); } catch (...) { automationTests_.clear(); } if (!std::ranges::any_of(automationTests_, [&](const auto& t){return t.id==selectedAutomationId_;})) selectedAutomationId_=automationTests_.empty()?0:automationTests_.front().id; }
    db::AutomationTest automationFromRequest(const db::SavedRequest& r, int vus, std::string duration) const { return {.projectId=currentProjectId_, .name=r.name, .method=r.method, .url=r.url, .params=r.params, .headers=r.headers, .cookies=r.cookies, .bodyKind=r.bodyKind, .body=r.body, .bodyContents=r.bodyContents, .followRedirects=r.followRedirects, .allowJsonComments=r.allowJsonComments, .vus=vus, .duration=std::move(duration)}; }

    // ---- 引擎状态 ----

    bool available() const { return engine_->available(); }
    std::string binaryPath() const { return engine_->binaryPath(); }
    bool running() const { return engine_->running(); }

    // ---- 命令 ----

    // 拉起压测。finalUrl 由调用方拼好（与单次发送同一套 query 逻辑）。
    void start(const api::RequestSpec& spec, const api::LoadOptions& opts,
               std::int64_t requestId, std::string name) {
        pendingRequestId_ = requestId;
        pendingName_ = std::move(name);
        pendingUrl_ = spec.url;
        pendingOpts_ = opts;
        engine_->start(spec, opts);
    }

    void stop() { engine_->stop(); }

    // UI 线程取新增输出行。
    std::vector<std::string> drainOutput() { return engine_->drainOutput(); }

    // UI 线程轮询：压测结束则落库并返回 true。
    bool pollSummary(api::LoadSummary& out) {
        if (!engine_->takeSummary(out)) return false;
        if (out.ok) {
            try {
                db_->addLoadRecord(db::LoadRecord{
                    .requestId = pendingRequestId_,
                    .name = pendingName_,
                    .url = pendingUrl_,
                    .vus = pendingOpts_.vus,
                    .duration = pendingOpts_.duration,
                    .requests = out.requests,
                    .rps = out.rps,
                    .p50Ms = out.p50Ms,
                    .p90Ms = out.p90Ms,
                    .p95Ms = out.p95Ms,
                    .p99Ms = out.p99Ms,
                    .failRate = out.failRate,
                    .createdAt = nowUnix(),
                });
            } catch (...) {
                // 落库失败不打断主流程
            }
        }
        return true;
    }

    std::vector<db::LoadRecord> records(int limit = 20) {
        try {
            return db_->listLoadRecords(limit);
        } catch (...) {
            return {};
        }
    }

private:
    std::unique_ptr<api::LoadEngine> engine_;
    std::unique_ptr<db::Db> db_;
    std::int64_t currentProjectId_ = 0;
    std::int64_t cachedProjectId_ = -1;
    std::int64_t selectedAutomationId_ = 0;
    std::vector<db::AutomationTest> automationTests_;

    // 在途压测的落库上下文。
    std::int64_t pendingRequestId_ = 0;
    std::string pendingName_;
    std::string pendingUrl_;
    api::LoadOptions pendingOpts_;
};

export LoadStore g_loadtest;  // 领域单例
