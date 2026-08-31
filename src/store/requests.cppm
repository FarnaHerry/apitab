// store/requests.cppm — 领域 store：组织 / 项目 / 请求集合 + 单次发送的唯一入口。
// UI 无关：不 import 任何 eui / ui.* 模块。持有（自保留）curl 引擎与 SQLite。
// 单次发送经引擎抽象 api::ApiEngine（当前为 curl 实现）执行：send 纯入队、
// 结果由 UI 侧轮询 takeResponse 取走；替换引擎实现只动本文件一处。
//
// 层级：组织(Org) → 项目(Project) → 请求(Request)。store 缓存当前组织/项目
// 与其请求列表；切换组织级联切到其第一个项目，切换项目重载请求列表。
// 打开的标签页是视图状态（store.ui），本 store 不关心。
//
// 纪律（对齐 tinynext store.tasks）：
//   - 引擎对象由本 store 持有，外部不直接碰 api::ApiEngine 指针；
//   - 除引擎内部工作线程外全部在 UI 线程调用；引擎结果槽内部有锁。
export module apitab.store.requests;

import std;
import apitab.api_engine;
import apitab.curl_engine;
import apitab.config;
import apitab.db;
import apitab.preferences;
import apitab.utils;
import nlohmann.json;

namespace {
std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// settings.ini 里 KV 列表的 JSON 文本 → KeyValue 数组（与 db 侧 {"k","v","on",
// "t","r"} 同格式；非法/非数组文本按空表处理）。
std::vector<api::KeyValue> parseSettingKv(const std::string& text) {
    std::vector<api::KeyValue> out;
    const nlohmann::json arr = nlohmann::json::parse(text, nullptr, false);
    if (!arr.is_array()) return out;
    for (const auto& item : arr) {
        if (!item.is_object()) continue;
        out.push_back({.key = item.value("k", ""),
                       .value = item.value("v", ""),
                       .enabled = item.value("on", true),
                       .type = item.value("t", ""),
                       .remark = item.value("r", "")});
    }
    return out;
}
} // namespace

export class RequestStore {
public:
    RequestStore()
        : engine_(makeCurlEngine()),
          db_(std::make_unique<db::Db>(cfg::databaseFile())) {
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
            (void)db_->createEnvironment(currentProjectId_, "localhost", "http://localhost");
            reloadProjects();
        });
    }

    std::string renameProject(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameProject(id, name);
            reloadProjects();
        });
    }

    // 项目设置页：全量更新名称/说明/公共请求头。
    std::string updateProjectMeta(std::int64_t id, const std::string& name,
                                  const std::string& description,
                                  const std::vector<api::KeyValue>& headers) {
        return guarded([&] {
            db_->updateProjectMeta(id, name, description, headers);
            reloadProjects();
        });
    }

    // ---- 全局设置（settings.ini KV；"全局设置"页写入，发送侧在 finalizeSpec
    // 统一生效；0/空 = 默认值）。k6 压测不经此路径（超时在脚本 options 里）。----
    static int globalTimeoutSec() {
        const std::string v = trim(sessionPreference("request_timeout_sec"));
        int out = 30;
        if (!v.empty()) {
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            if (ec != std::errc{} || ptr != v.data() + v.size()) out = 30;
        }
        return out > 0 ? out : 30;
    }
    static std::string globalProxy() { return trim(sessionPreference("request_proxy")); }
    static std::vector<api::KeyValue> globalHeaders() {
        return parseSettingKv(sessionPreference("global_headers"));
    }

    // 删除项目（级联删其请求）。
    std::string deleteProject(std::int64_t id) {
        return guarded([&] {
            db_->deleteProject(id);
            db_->ensureDefaultProject();
            reloadProjects();
        });
    }

    std::int64_t currentEnvForProject(std::int64_t projectId) const {
        if (const auto it = selectedEnvByProject_.find(projectId); it != selectedEnvByProject_.end()) return it->second;
        return projectId == currentProjectId_ ? currentEnvId_ : 0;
    }
    std::string restoreEnvironment(std::int64_t projectId, std::int64_t envId) {
        if (const std::string err = selectProject(projectId); !err.empty()) return err;
        return selectEnv(envId);
    }



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

    std::string setEnvironmentVariables(std::int64_t id,
                                        const std::vector<api::KeyValue>& vars) {
        return guarded([&] {
            db_->setEnvironmentVariables(id, vars);
            reloadEnvironments();
        });
    }

    std::string updateEnvironment(std::int64_t id, const std::string& name,
                                  const std::string& baseUrl,
                                  const std::vector<api::KeyValue>& variables) {
        return guarded([&] {
            db_->renameEnvironment(id, name);
            db_->setEnvironmentBaseUrl(id, baseUrl);
            db_->setEnvironmentVariables(id, variables);
            reloadEnvironments();
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
            appendSegment(groupPathValue(*g));
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
            if (g.mode == db::GroupMode::Path && groupPathValue(g) == db::groupPath(segs)) {
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
            return "/" + groupPathValue(*g);  // "api/v1" → "/api/v1"
        }
        return {};
    }

    // Path 分组的实际 URL 前缀：path 列空时回落 name（兼容旧数据）。
    static std::string groupPathValue(const db::Group& g) {
        return g.path.empty() ? g.name : g.path;
    }

    std::string createGroup(const std::string& name, db::GroupMode mode,
                            std::int64_t parentId = 0, const std::string& path = "") {
        return guarded([&] {
            if (parentId != 0) {
                const db::Group* parent = findGroup(parentId);
                if (!parent || parent->projectId != currentProjectId_) {
                    throw std::runtime_error("父目录不属于当前项目");
                }
            }
            db_->createGroup(currentProjectId_, name, mode, parentId, path);
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

    // 分组换父（拖拽用，0 = 移到根目录）。环检测：目标不能是自身或自身的后代。
    std::string moveGroup(std::int64_t id, std::int64_t parentId) {
        return guarded([&] {
            if (!findGroup(id)) throw std::runtime_error("分组不存在");
            if (parentId != 0) {
                if (!findGroup(parentId)) throw std::runtime_error("目标分组不存在");
                // 沿目标的父链上溯，撞到 id 即成环（含 parentId == id 自身）。
                std::int64_t cur = parentId;
                while (cur != 0) {
                    if (cur == id) throw std::runtime_error("不能移动到自身或其子分组下");
                    const db::Group* g = findGroup(cur);
                    cur = g != nullptr ? g->parentId : 0;
                }
            }
            db_->setGroupParent(id, parentId);
            reloadGroups();
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
        if (currentProjectId_ == 0) return "未打开项目，无法保存";
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

    // 重命名已保存请求：改字段走 saveRequest 更新（同 moveToGroup 的思路）。
    std::string renameRequest(std::int64_t id, const std::string& name) {
        return guarded([&] {
            if (const db::SavedRequest* r = find(id)) {
                db::SavedRequest copy = *r;
                copy.name = name;
                db_->saveRequest(copy);
                reloadRequests();
            }
        });
    }

    std::vector<db::GlobalCookie> globalCookies() const {
        try { return db_->listGlobalCookies(currentProjectId_); } catch (...) { return {}; }
    }
    std::string saveGlobalCookie(db::GlobalCookie& cookie) {
        if (cookie.projectId != 0 && cookie.projectId != currentProjectId_) return "项目不匹配";
        cookie.projectId = currentProjectId_;
        try {
            cookie.id = db_->saveGlobalCookie(cookie);
            return {};
        } catch (const std::exception& e) { return e.what(); }
    }
    std::string deleteGlobalCookie(std::int64_t id) {
        try { db_->deleteGlobalCookie(id, currentProjectId_); return {}; }
        catch (const std::exception& e) { return e.what(); }
    }

    // ---- 发送（传输由引擎抽象 api::ApiEngine 执行，当前为 curl 实现）----

    // 派发一次请求（异步）：引擎 send 纯入队立即返回，结果由 UI 侧轮询
    // takeResponse 取走；busy 时再调视为替换（取消旧请求、发起新请求）。
    void sendViaEngine(const api::RequestSpec& spec) { engine_->send(spec); }
    // 协作式取消在途请求（不保证立即生效）；丢弃排队请求，取消后结果不投递。
    void cancelSend() { engine_->cancel(); }
    // UI 线程轮询：引擎有新完成的结果则取出并返回 true（一个结果只取一次）。
    bool takeResponse(api::ResponseView& out) { return engine_->takeResponse(out); }

    // 组装最终请求规格：环境变量替换 + 基础 URL 拼接 + url 拼启用的 query 参数，
    // 合并全局 Cookie。
    // 先替换当前环境的 {{变量}}，再拼 URL（保证变量值里的特殊字符被正确百分号编码）。
    api::RequestSpec finalizeSpec(const api::RequestSpec& spec) {
        api::RequestSpec finalSpec = spec;
        // 环境变量替换：当前环境启用的 {{name}} 应用到 url / params / headers /
        // cookies / body；未定义或停用的占位符保留原样。全局 Cookie 是项目级静态
        // 值，在替换之后合并，不参与变量替换。
        if (const db::Environment* env = findEnvironment(currentEnvId_);
            env && !env->variables.empty()) {
            const std::vector<api::KeyValue>& vars = env->variables;
            finalSpec.url = substituteEnvVars(finalSpec.url, vars);
            for (auto& p : finalSpec.params) {
                p.key = substituteEnvVars(p.key, vars);
                p.value = substituteEnvVars(p.value, vars);
            }
            for (auto& h : finalSpec.headers) {
                h.key = substituteEnvVars(h.key, vars);
                h.value = substituteEnvVars(h.value, vars);
            }
            for (auto& c : finalSpec.cookies) {
                c.key = substituteEnvVars(c.key, vars);
                c.value = substituteEnvVars(c.value, vars);
            }
            finalSpec.body = substituteEnvVars(finalSpec.body, vars);
        }
        // 基础 URL 拼接：输入（变量替换后）无 URI scheme 且当前环境配置了
        // baseUrl 时拼上前缀（composeUrl 内部分段去重斜杠；请求页草稿没有分组
        // 概念，groupId 传 0 跳过 Path 分组）。带 scheme 的完整 URL 原样保留。
        finalSpec.url = composeUrl(finalSpec.url, 0, currentEnvId_);
        std::vector<std::pair<std::string, std::string>> enabled;
        for (const auto& p : finalSpec.params) {
            if (p.enabled && !p.key.empty()) enabled.emplace_back(p.key, p.value);
        }
        for (const auto& cookie : globalCookies()) {
            if (cookie.enabled) finalSpec.cookies.push_back({cookie.name, cookie.value, true, {}, {}});
        }
        // JSON 体剥离注释（引擎不处理，见 curl_engine.cpp；原由已删除的
        // http_build 承担，随发送切回引擎移入本函数）。
        if (finalSpec.bodyKind == api::BodyKind::Json && finalSpec.allowJsonComments) {
            finalSpec.body = stripJsonComments(finalSpec.body);
        }
        finalSpec.url = appendQuery(trim(finalSpec.url), enabled);
        // ---- 项目/全局公共头合并 + 全局超时/代理注入 ----
        // 同名键（大小写不敏感）请求显式头优先，项目头优先于全局头；环境变量
        // 替换已在前面完成，公共头不参与 {{var}} 替换（配置侧直接写死值）。
        std::vector<api::KeyValue> inherited;
        for (const db::Project& p : projects_) {
            if (p.id == currentProjectId_)
                for (const api::KeyValue& h : p.headers) inherited.push_back(h);
        }
        for (const api::KeyValue& h : globalHeaders()) inherited.push_back(h);
        for (const api::KeyValue& h : inherited) {
            if (!h.enabled || h.key.empty()) continue;
            const std::string want = lowerAscii(h.key);
            const bool exists =
                std::any_of(finalSpec.headers.begin(), finalSpec.headers.end(),
                            [&](const api::KeyValue& e) {
                                return e.enabled && !e.key.empty() && lowerAscii(e.key) == want;
                            });
            if (!exists) finalSpec.headers.push_back(h);
        }
        finalSpec.timeoutSec = globalTimeoutSec();
        finalSpec.proxy = globalProxy();
        return finalSpec;
    }

    // 历史落库：响应回到 UI 线程后由页面调用；写入失败不打断主流程。
    // requestId 关联集合请求（未保存的草稿 = 0）。
    void recordHistory(std::int64_t requestId, const std::string& method,
                       const std::string& url, const api::ResponseView& result) {
        try {
            db_->addHistory(db::HistoryEntry{
                .requestId = requestId,
                .method = method,
                .url = url,
                .status = result.status,
                .durationMs = result.totalMs,
                .sizeBytes = result.sizeBytes,
                .error = result.error,
                .createdAt = nowUnix(),
            });
        } catch (...) {
            // 历史写入失败不打断主流程
        }
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
    // 剥离 JSON 的 // 与 /* */ 注释（字符串字面量内除外），供 finalizeSpec 在
    // 发送前处理 allowJsonComments 的 JSON body。
    static std::string stripJsonComments(std::string_view input) {
        std::string output;
        output.reserve(input.size());
        bool inString = false;
        bool escaped = false;
        for (std::size_t i = 0; i < input.size();) {
            const char c = input[i];
            if (inString) {
                output.push_back(c);
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                ++i;
                continue;
            }
            if (c == '"') {
                inString = true;
                output.push_back(c);
                ++i;
            } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '/') {
                i += 2;
                while (i < input.size() && input[i] != '\n') ++i;
            } else if (c == '/' && i + 1 < input.size() && input[i + 1] == '*') {
                i += 2;
                while (i + 1 < input.size() && !(input[i] == '*' && input[i + 1] == '/')) ++i;
                if (i + 1 < input.size()) i += 2;
            } else {
                output.push_back(c);
                ++i;
            }
        }
        return output;
    }

    // 把启用的变量 {{key}} 替换为其 value；空 key、停用或未定义的占位符保留原样。
    // 按变量表顺序逐个替换，不递归解析替换结果里的新占位符。
    static std::string substituteEnvVars(std::string text,
                                         const std::vector<api::KeyValue>& vars) {
        for (const auto& v : vars) {
            if (!v.enabled || v.key.empty()) continue;
            const std::string token = "{{" + v.key + "}}";
            std::size_t pos = 0;
            while ((pos = text.find(token, pos)) != std::string::npos) {
                text.replace(pos, token.size(), v.value);
                pos += v.value.size();
            }
        }
        return text;
    }

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
        // 0 = 用户尚未打开项目；失效（被删/切组织）时显式回落为 0，不静默挑选。
        const bool valid = std::ranges::any_of(projects_, [&](const db::Project& p) {
            return p.id == currentProjectId_;
        });
        if (!valid) currentProjectId_ = 0;
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

    std::unique_ptr<api::ApiEngine> engine_;  // 单次发送引擎（自保留，工作线程自管）
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
};

export RequestStore g_requests;  // 领域单例（importers 间共享同一实体）
