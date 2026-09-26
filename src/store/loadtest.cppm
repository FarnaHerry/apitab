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
    // 与 RequestStore 同理：静态初始化保持廉价，打开是显式的（见 requests.cppm）。
    // 否则 CLI 客户端进程仅因模块级全局构造就会打开数据库。引擎（k6 路径解析）也在
    // Open 里做——压测结果落库属于 GUI 进程的事。
    LoadStore() = default;

    // 打开库与 k6 引擎（幂等）。GUI 进程在进事件循环前调用一次。
    void Open() {
        if (opened_) return;
        opened_ = true;
        engine_ = makeK6Engine(cfg::k6Binary().string());
        const Status initialized = captureResult([&] {
            db_ = std::make_unique<db::Db>(cfg::databaseFile());
        });
        if (!initialized) startupError_ = initialized.error();
    }

    [[nodiscard]] bool opened() const { return opened_; }

    LoadStore(const LoadStore&) = delete;
    LoadStore& operator=(const LoadStore&) = delete;

    Status setProject(std::int64_t projectId) {
        currentProjectId_ = projectId;
        return reloadAutomation();
    }
    Result<std::reference_wrapper<const std::vector<db::AutomationTest>>> automationTests() {
        if (Status loaded = reloadAutomation(); !loaded) return std::unexpected(loaded.error());
        return std::cref(automationTests_);
    }
    Result<const db::AutomationTest*> selectedAutomation() {
        if (Status loaded = reloadAutomation(); !loaded) return std::unexpected(loaded.error());
        for (const auto& test : automationTests_)
            if (test.id == selectedAutomationId_) return &test;
        return static_cast<const db::AutomationTest*>(nullptr);
    }
    std::int64_t selectedAutomationId() const { return selectedAutomationId_; }
    void selectAutomation(std::int64_t id) { selectedAutomationId_ = id; }
    Status saveAutomation(db::AutomationTest& t) {
        t.projectId = currentProjectId_;
        t.updatedAt = nowUnix();
        const auto saved = databaseResult([&] { return db_->saveAutomationTest(t); });
        if (!saved) return std::unexpected(saved.error());
        t.id = *saved;
        selectedAutomationId_ = t.id;
        return reloadAutomation();
    }
    Status removeAutomation(std::int64_t id) {
        const Status removed = databaseResult([&] {
            db_->deleteAutomationTest(id, currentProjectId_);
        });
        if (!removed) return removed;
        if (selectedAutomationId_ == id) selectedAutomationId_ = 0;
        return reloadAutomation();
    }
    Status reloadAutomation() {
        if (currentProjectId_ == cachedProjectId_) return {};
        const auto loaded = databaseResult([&] {
            return db_->listAutomationTests(currentProjectId_);
        });
        if (!loaded) return std::unexpected(loaded.error());
        automationTests_ = *loaded;
        cachedProjectId_ = currentProjectId_;
        if (!std::ranges::any_of(automationTests_, [&](const auto& test) {
                return test.id == selectedAutomationId_;
            })) {
            selectedAutomationId_ = automationTests_.empty() ? 0 : automationTests_.front().id;
        }
        return {};
    }
    db::AutomationTest automationFromRequest(const db::SavedRequest& r, int vus, std::string duration) const { return {.projectId=currentProjectId_, .name=r.name, .method=r.method, .url=r.url, .params=r.params, .headers=r.headers, .cookies=r.cookies, .bodyKind=r.bodyKind, .body=r.body, .bodyContents=r.bodyContents, .followRedirects=r.followRedirects, .allowJsonComments=r.allowJsonComments, .vus=vus, .duration=std::move(duration)}; }

    // ---- 引擎状态 ----

    bool available() const { return engine_->available(); }
    std::string binaryPath() const { return engine_->binaryPath(); }
    bool running() const { return engine_->running(); }

    // 按请求参数生成 k6 脚本模板（压测页脚本编辑器的初始内容/重新生成）。
    std::string scriptTemplate(const api::RequestSpec& spec, const api::LoadOptions& opts) const {
        return api::BuildScript(spec, opts);
    }

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
    Result<bool> pollSummary(api::LoadSummary& out) {
        if (!engine_->takeSummary(out)) return false;
        if (out.ok) {
            const Status saved = databaseResult([&] {
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
            });
            if (!saved) return std::unexpected(saved.error());
        }
        return true;
    }

    Result<std::vector<db::LoadRecord>> records(int limit = 20) {
        return databaseResult([&] { return db_->listLoadRecords(limit); });
    }

private:
    template <typename F>
    auto databaseResult(F&& fn) -> Result<std::invoke_result_t<F>> {
        if (!db_) return std::unexpected(startupError_.value_or(AppError{"数据库不可用"}));
        return captureResult(std::forward<F>(fn));
    }

    std::unique_ptr<api::LoadEngine> engine_;
    std::unique_ptr<db::Db> db_;
    std::int64_t currentProjectId_ = 0;
    std::int64_t cachedProjectId_ = -1;
    std::int64_t selectedAutomationId_ = 0;
    std::vector<db::AutomationTest> automationTests_;
    std::optional<AppError> startupError_;
    bool opened_ = false;

    // 在途压测的落库上下文。
    std::int64_t pendingRequestId_ = 0;
    std::string pendingName_;
    std::string pendingUrl_;
    api::LoadOptions pendingOpts_;
};

// 领域单例。打开同样是显式的：GUI 进程进事件循环前 Open()，CLI 客户端不调用。
export LoadStore g_loadtest;
