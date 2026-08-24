// store/requests.cppm — 领域 store：组织 / 项目 / 请求集合 + 单次发送的唯一入口。
// UI 无关：不 import 任何 eui / ui.* 模块。持有（自保留）curl 引擎与 SQLite。
//
// 层级：组织(Org) → 项目(Project) → 请求(Request)。store 缓存当前组织/项目
// 与其请求列表；切换组织级联切到其第一个项目，切换项目重载请求列表。
// 打开的标签页是视图状态（store.ui），本 store 不关心。
//
// 纪律（对齐 tinynext store.tasks）：
//   - 引擎对象由本 store 持有，外部不直接碰 api::ApiEngine 指针；
//   - send 只负责派发与记录历史，结果展示是 UI 层的事（经 pollResult 取走）；
//   - 除引擎内部工作线程外全部在 UI 线程调用；引擎结果槽内部有锁。
export module apitab.store.requests;

import std;
import apitab.api_engine;
import apitab.curl_engine;
import apitab.config;
import apitab.db;
import apitab.utils;

export class RequestStore {
public:
    RequestStore()
        : engine_(makeCurlEngine()), db_(std::make_unique<db::Db>(cfg::databaseFile())) {
        try {
            // 迁移兜底：空库建默认组织/项目，游离请求归入默认项目。
            db_->ensureDefaultProject();
            reloadOrgs();
            healthy_ = true;
        } catch (...) {
            healthy_ = false;
        }
    }

    RequestStore(const RequestStore&) = delete;
    RequestStore& operator=(const RequestStore&) = delete;

    // ---- 组织 ----

    const std::vector<db::Org>& orgs() const { return orgs_; }
    std::int64_t currentOrgId() const { return currentOrgId_; }

    std::string selectOrg(std::int64_t id) {
        return guarded([&] {
            if (currentProjectId_ != 0) {
                selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            }
            currentOrgId_ = id;
            reloadProjects();
        });
    }

    std::string createOrg(const std::string& name) {
        return guarded([&] {
            currentOrgId_ = db_->createOrg(name);
            reloadOrgs();
        });
    }

    std::string renameOrg(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameOrg(id, name);
            reloadOrgs();
        });
    }

    // 删除组织（级联删项目与请求）。返回错误消息或空串。
    std::string deleteOrg(std::int64_t id) {
        return guarded([&] {
            db_->deleteOrg(id);
            db_->ensureDefaultProject();  // 删光后保底
            reloadOrgs();
        });
    }

    // ---- 项目 ----

    const std::vector<db::Project>& projects() const { return projects_; }
    std::vector<db::Project> allProjects() const {
        std::vector<db::Project> result;
        for (const auto& org : orgs_) {
            std::vector<db::Project> projects = db_->listProjects(org.id);
            result.insert(result.end(), std::make_move_iterator(projects.begin()),
                          std::make_move_iterator(projects.end()));
        }
        return result;
    }
    std::int64_t currentProjectId() const { return currentProjectId_; }

    std::string selectProjectInOrg(std::int64_t orgId, std::int64_t projectId) {
        return guarded([&] {
            const std::vector<db::Project> candidates = db_->listProjects(orgId);
            const bool belongs = std::ranges::any_of(candidates, [&](const db::Project& p) {
                return p.id == projectId;
            });
            if (!belongs) throw std::runtime_error("项目不属于指定组织");
            if (currentProjectId_ != 0) selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            currentOrgId_ = orgId;
            projects_ = candidates;
            currentProjectId_ = projectId;
            reloadGroups();
        });
    }

    std::string selectProject(std::int64_t id) {
        return guarded([&] {
            if (currentProjectId_ != 0) {
                selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            }
            currentProjectId_ = id;
            reloadGroups();
        });
    }

    std::string createProject(const std::string& name) {
        return guarded([&] {
            currentProjectId_ = db_->createProject(currentOrgId_, name);
            reloadProjects();
        });
    }

    std::string renameProject(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameProject(id, name);
            reloadProjects();
        });
    }

    // 删除项目（级联删其请求）。
    std::string deleteProject(std::int64_t id) {
        return guarded([&] {
            db_->deleteProject(id);
            db_->ensureDefaultProject();
            reloadProjects();
        });
    }

    // ---- 分组（当前项目的）----

    const std::vector<db::Group>& groups() const { return groups_; }
    const std::vector<db::Environment>& environments() const { return environments_; }

    const db::Group* findGroup(std::int64_t id) const {
        for (const auto& g : groups_) {
            if (g.id == id) return &g;
        }
        return nullptr;
    }

    const db::Environment* findEnvironment(std::int64_t id) const {
        for (const auto& e : environments_) {
            if (e.id == id) return &e;
        }
        return nullptr;
    }

    // 当前项目的环境列表（id 升序）。
    // 默认第一个为当前环境，下标 0 提供一个"无"选项。
    std::int64_t currentEnvId() const { return currentEnvId_; }
    std::string selectEnv(std::int64_t id) {
        return guarded([&] {
            if (id != 0 && !findEnvironment(id)) {
                throw std::runtime_error("环境不属于当前项目");
            }
            currentEnvId_ = id;
            selectedEnvByProject_[currentProjectId_] = id;
        });
    }

    std::string createEnvironment(const std::string& name, const std::string& baseUrl) {
        return guarded([&] {
            currentEnvId_ = db_->createEnvironment(currentProjectId_, name, baseUrl);
            selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            reloadEnvironments();
        });
    }

    std::string renameEnvironment(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameEnvironment(id, name);
            reloadEnvironments();
        });
    }

    std::string setEnvironmentBaseUrl(std::int64_t id, const std::string& baseUrl) {
        return guarded([&] {
            db_->setEnvironmentBaseUrl(id, baseUrl);
            reloadEnvironments();
        });
    }

    std::string updateEnvironment(std::int64_t id, const std::string& name,
                                  const std::string& baseUrl) {
        return guarded([&] {
            db_->renameEnvironment(id, name);
            db_->setEnvironmentBaseUrl(id, baseUrl);
        });
    }

    std::string deleteEnvironment(std::int64_t id) {
        return guarded([&] {
            db_->deleteEnvironment(id);
            reloadEnvironments();
        });
    }

    // 组装最终 URL：baseUrl + groupUrlPrefix + path
    std::string composeUrl(const std::string& path, std::int64_t groupId,
                           std::int64_t envId) const {
        const std::string cleanPath = trim(path);
        // 显式 URI scheme 是完整目标，不再叠加环境或 Path 分组。
        if (hasUriScheme(cleanPath)) return cleanPath;

        std::string url;
        if (const db::Environment* e = findEnvironment(envId)) {
            url = trim(e->baseUrl);
        }
        auto appendSegment = [&](std::string_view segment) {
            while (!segment.empty() && segment.front() == '/') segment.remove_prefix(1);
            while (!url.empty() && url.back() == '/') url.pop_back();
            if (segment.empty()) return;
            if (!url.empty()) url += '/';
            url += segment;
        };
        if (const db::Group* g = findGroup(groupId); g && g->mode == db::GroupMode::Path) {
            appendSegment(g->name);
        }
        appendSegment(cleanPath);
        return url;
    }

    // 把一组名字段（如 {"api","v1"}）拼成 Path 分组的路径字符串。
    // 名字里可能含 '/';Path 模式时按 '/' 展开，Name 模式折成单层。
    // 这里只负责拼字符串，查找由 resolveGroup 完成。
    static std::string segmentsToPath(const std::vector<std::string>& segs) {
        return db::groupPath(segs);
    }

    // 给一条路径（如 "api/v1"）在 Path 分组里找到对应的分组 id。
    // 逐级比较每段名字。返回 0 = 找不到。
    std::int64_t resolveGroupId(const std::vector<std::string>& segs) const {
        if (segs.empty()) return 0;
        for (const auto& g : groups_) {
            if (g.mode == db::GroupMode::Path && g.name == db::groupPath(segs)) {
                return g.id;
            }
        }
        return 0;
    }

    // 查分组的所有祖先路径（从根到叶）。如分组 "api/v1" → [{"api"},{"api","v1"}]。
    // 供 URL 拼接：拼接 = "/" + groupPath(ancestors[0]) + ... + "/" + name。
    // 这里简化：只取第一层 Path 分组的路径（忽略嵌套），因为目前只支持单层 Path。
    std::string groupUrlPrefix(std::int64_t groupId) const {
        if (const db::Group* g = findGroup(groupId); g && g->mode == db::GroupMode::Path) {
            return "/" + g->name;  // "api/v1" → "/api/v1"
        }
        return {};
    }

    std::string createGroup(const std::string& name, db::GroupMode mode,
                            std::int64_t parentId = 0) {
        return guarded([&] {
            if (parentId != 0) {
                const db::Group* parent = findGroup(parentId);
                if (!parent || parent->projectId != currentProjectId_) {
                    throw std::runtime_error("父目录不属于当前项目");
                }
            }
            db_->createGroup(currentProjectId_, name, mode, parentId);
            reloadGroups();
        });
    }

    std::string renameGroup(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameGroup(id, name);
            reloadGroups();
        });
    }

    std::string setGroupMode(std::int64_t id, db::GroupMode mode) {
        return guarded([&] {
            db_->setGroupMode(id, mode);
            reloadGroups();
        });
    }

    std::string deleteGroup(std::int64_t id) {
        return guarded([&] {
            db_->deleteGroup(id);
            reloadGroups();
        });
    }

    // 把请求移到某分组（0 = 未分组）。
    std::string moveToGroup(std::int64_t requestId, std::int64_t groupId) {
        return guarded([&] {
            // 改字段走 saveRequest 更新。这里直接 SQL 更快：复用 db 的 save 逻辑，
            // 读出来改 groupId 再 save。代价一次 select+update，可接受。
            if (const db::SavedRequest* r = find(requestId)) {
                db::SavedRequest copy = *r;
                copy.groupId = groupId;
                db_->saveRequest(copy);
                reloadRequests();
            }
        });
    }

    // ---- 集合（当前项目的请求）----

    const std::vector<db::SavedRequest>& list() const { return requests_; }

    const db::SavedRequest* find(std::int64_t id) const {
        for (const auto& r : requests_) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

    // 保存（新建 id==0 / 更新）。失败返回错误消息，成功返回空串。
    std::string save(db::SavedRequest& r) {
        r.projectId = currentProjectId_;
        r.updatedAt = nowUnix();
        try {
            const std::int64_t id = db_->saveRequest(r);
            r.id = id;
            reloadRequests();
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    }

    std::string remove(std::int64_t id) {
        try {
            db_->deleteRequest(id);
            reloadRequests();
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    }

    // ---- 发送 ----

    // 派发一次请求（异步）。finalUrl = url + 启用的 query 参数。requestId 用于
    // 历史记录关联（未保存的草稿 = 0）。
    void send(const api::RequestSpec& spec, std::int64_t requestId) {
        pendingMethod_ = spec.method;
        pendingRequestId_ = requestId;
        api::RequestSpec finalSpec = spec;
        std::vector<std::pair<std::string, std::string>> enabled;
        for (const auto& p : spec.params) {
            if (p.enabled && !p.key.empty()) enabled.emplace_back(p.key, p.value);
        }
        finalSpec.url = appendQuery(trim(spec.url), enabled);
        pendingUrl_ = finalSpec.url;
        engine_->send(finalSpec);
    }

    bool busy() const { return engine_->busy(); }
    void cancel() { engine_->cancel(); }

    // UI 线程轮询：引擎有新结果则记录历史并返回 true。
    bool pollResult(api::ResponseView& out) {
        if (!engine_->takeResponse(out)) return false;
        try {
            db_->addHistory(db::HistoryEntry{
                .requestId = pendingRequestId_,
                .method = pendingMethod_,
                .url = pendingUrl_,
                .status = out.status,
                .durationMs = out.totalMs,
                .sizeBytes = out.sizeBytes,
                .error = out.error,
                .createdAt = nowUnix(),
            });
        } catch (...) {
            // 历史写入失败不打断主流程
        }
        return true;
    }

    std::vector<db::HistoryEntry> history(int limit = 50) {
        try {
            return db_->listHistory(limit);
        } catch (...) {
            return {};
        }
    }

    std::int64_t historyCount() {
        try {
            return db_->historyCount();
        } catch (...) {
            return 0;
        }
    }

    std::vector<db::HistoryEntry> historyPage(int pageSize, std::int64_t pageIndex) {
        try {
            const int safePageSize = std::clamp(pageSize, 1, 100);
            const std::int64_t offset = std::max<std::int64_t>(0, pageIndex) * safePageSize;
            return db_->listHistoryPage(safePageSize, offset);
        } catch (...) {
            return {};
        }
    }

    void clearHistory() {
        try {
            db_->clearHistory();
        } catch (...) {
        }
    }

    // DB 打开失败（目录不可写等）：返回 false，集合功能整体降级为空。
    bool healthy() const { return healthy_; }

private:
    // CRUD 包装：统一 try/catch → 错误消息。
    template <typename F>
    std::string guarded(F&& fn) {
        try {
            fn();
            healthy_ = true;
            return {};
        } catch (const std::exception& e) {
            healthy_ = false;
            return e.what();
        }
    }

    void reloadOrgs() {
        orgs_ = db_->listOrgs();
        // 当前组织失效（被删/首启）→ 选第一个。
        const bool valid = std::ranges::any_of(orgs_, [&](const db::Org& o) {
            return o.id == currentOrgId_;
        });
        if (!valid && !orgs_.empty()) currentOrgId_ = orgs_.front().id;
        reloadProjects();
    }

    void reloadProjects() {
        projects_ = db_->listProjects(currentOrgId_);
        const bool valid = std::ranges::any_of(projects_, [&](const db::Project& p) {
            return p.id == currentProjectId_;
        });
        if (!valid && !projects_.empty()) currentProjectId_ = projects_.front().id;
        reloadGroups();
    }

    void reloadGroups() {
        groups_ = currentProjectId_ != 0 ? db_->listGroups(currentProjectId_)
                                          : std::vector<db::Group>{};
        reloadEnvironments();
    }

    void reloadEnvironments() {
        environments_ = currentProjectId_ != 0 ? db_->listEnvironments(currentProjectId_)
                                                : std::vector<db::Environment>{};
        if (const auto it = selectedEnvByProject_.find(currentProjectId_);
            it != selectedEnvByProject_.end()) {
            currentEnvId_ = it->second;
        }
        // 当前环境失效 → 选第一个
        const bool valid = std::ranges::any_of(environments_, [&](const db::Environment& e) {
            return e.id == currentEnvId_;
        });
        if (!valid && !environments_.empty()) currentEnvId_ = environments_.front().id;
        else if (environments_.empty()) currentEnvId_ = 0;
        selectedEnvByProject_[currentProjectId_] = currentEnvId_;
        reloadRequests();
    }

    void reloadRequests() {
        requests_ = currentProjectId_ != 0 ? db_->listRequests(currentProjectId_)
                                           : std::vector<db::SavedRequest>{};
    }

    std::unique_ptr<api::ApiEngine> engine_;
    std::unique_ptr<db::Db> db_;

    std::vector<db::Org> orgs_;
    std::vector<db::Project> projects_;
    std::vector<db::Group> groups_;
    std::vector<db::Environment> environments_;
    std::vector<db::SavedRequest> requests_;
    std::int64_t currentOrgId_ = 0;
    std::int64_t currentProjectId_ = 0;
    std::int64_t currentEnvId_ = 0;
    std::unordered_map<std::int64_t, std::int64_t> selectedEnvByProject_;
    bool healthy_ = true;

    // 在途请求的历史记录上下文（send 时记下，pollResult 时落库）。
    std::int64_t pendingRequestId_ = 0;
    std::string pendingMethod_;
    std::string pendingUrl_;
};

export RequestStore g_requests;  // 领域单例（importers 间共享同一实体）
