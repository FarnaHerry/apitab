// db.cppm — apitab.db：SQLite 持久化（接口模块）。
//
// 三张表：
//   requests    —— 请求集合（侧栏列表；params/headers 以 JSON 数组存）
//   history     —— 单次请求历史（每次 Send 落一条）
//   load_tests  —— 压测记录（每次 k6 跑完落一条）
// SQLiteCpp 头只进实现单元（db.cpp 全局模块片段），接口只暴露纯数据结构。
export module apitab.db;

import std;
import apitab.api_engine;

namespace db {

export struct Org {
    std::int64_t id = 0;
    std::string name;
};

export struct Project {
    std::int64_t id = 0;
    std::int64_t orgId = 0;
    std::string name;
};

// 请求分组（项目内）。mode 决定 url 拼接方式：
//   Name — 仅名称（显示/组织作用，不参与 URL）；
//   Path — 按斜杠展开成 API 路径前缀（输入 users → 实际 /api/v1/users）。
export enum class GroupMode { Name, Path };
export const char* groupModeName(GroupMode mode);  // "仅名称" / "路径"

export struct Group {
    std::int64_t id = 0;
    std::int64_t projectId = 0;
    std::int64_t parentId = 0;          // 0 = 根目录
    std::string name;
    GroupMode mode = GroupMode::Name;
};

// 环境（项目内）。baseUrl 作为该项目所有请求的最终 URL 前缀，
// 如 "http://localhost:8080/" —— 请求 URL 只填相对路径即可。
export struct Environment {
    std::int64_t id = 0;
    std::int64_t projectId = 0;
    std::string name;
    std::string baseUrl;
};

// 全路径：a/b/c → "a/b/c"。Path 分组才展开，Name 分组折成单层。
export std::string groupPath(const std::vector<std::string>& segs);

export struct SavedRequest {
    std::int64_t id = 0;                 // 0 = 未保存过
    std::int64_t projectId = 0;          // 所属项目
    std::int64_t groupId = 0;            // 所属分组；0 = 未分组
    std::string name;
    api::RequestKind kind = api::RequestKind::Http;
    std::string wsProtocol;
    std::string method = "GET";
    std::string url;
    std::vector<api::KeyValue> params;
    std::vector<api::KeyValue> headers;
    std::vector<api::KeyValue> cookies;
    api::BodyKind bodyKind = api::BodyKind::None;
    std::string body;                    // legacy/current text body
    std::array<api::BodyContent, 7> bodyContents{};
    bool followRedirects = true;
    bool allowJsonComments = true;
    std::int64_t updatedAt = 0;
};

export struct GlobalCookie {
    std::int64_t id = 0;
    std::int64_t projectId = 0;
    std::string name;
    std::string value;
    bool enabled = true;
};

export struct HistoryEntry {
    std::int64_t id = 0;
    std::int64_t requestId = 0;          // 关联集合项；未保存的请求 = 0
    std::string method;
    std::string url;
    long status = 0;
    double durationMs = 0;
    std::int64_t sizeBytes = 0;
    std::string error;
    std::int64_t createdAt = 0;
};

export struct AutomationTest {
    std::int64_t id = 0;
    std::int64_t projectId = 0;
    std::string name = "未命名自动化测试";
    std::string method = "GET";
    std::string url;
    std::vector<api::KeyValue> params;
    std::vector<api::KeyValue> headers;
    std::vector<api::KeyValue> cookies;
    api::BodyKind bodyKind = api::BodyKind::None;
    std::string body;
    std::array<api::BodyContent, 7> bodyContents{};
    bool followRedirects = true;
    bool allowJsonComments = true;
    int vus = 10;
    std::string duration = "30s";
    std::int64_t updatedAt = 0;
};

export struct LoadRecord {
    std::int64_t id = 0;
    std::int64_t requestId = 0;
    std::string name;                    // 请求名（快照，删了集合项也能看）
    std::string url;
    int vus = 0;
    std::string duration;
    std::int64_t requests = 0;
    double rps = 0;
    double p50Ms = 0, p90Ms = 0, p95Ms = 0, p99Ms = 0;
    double failRate = 0;
    std::int64_t createdAt = 0;
};

// 数据库句柄。构造即打开 + 建表（幂等）。所有方法抛 std::runtime_error；
// 调用方（领域 store）负责兜底转状态消息。
export class Db {
public:
    explicit Db(const std::filesystem::path& file);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // ---- organizations / projects（层级：组织 → 项目 → 请求）----
    std::vector<Org> listOrgs();                            // id 升序（创建顺序）
    std::int64_t createOrg(const std::string& name);
    void renameOrg(std::int64_t id, const std::string& name);
    void deleteOrg(std::int64_t id);                        // 级联删项目与其请求

    std::vector<Project> listProjects(std::int64_t orgId);  // id 升序
    std::int64_t createProject(std::int64_t orgId, const std::string& name);
    void renameProject(std::int64_t id, const std::string& name);
    void deleteProject(std::int64_t id);                    // 级联删其请求

    // 保底：库为空时自动建「默认组织 / 默认项目」；返回任一可用项目 id。
    std::int64_t ensureDefaultProject();

    // ---- groups（项目内分组；请求的分组决定侧栏层级与 URL 前缀）----
    std::vector<Group> listGroups(std::int64_t projectId);       // id 升序
    std::int64_t createGroup(std::int64_t projectId, const std::string& name, GroupMode mode,
                             std::int64_t parentId = 0);
    void renameGroup(std::int64_t id, const std::string& name);
    void setGroupMode(std::int64_t id, GroupMode mode);
    void deleteGroup(std::int64_t id);                           // 其请求置为未分组

    // ---- environments（项目内环境；请求 URL 前缀）----
    std::vector<Environment> listEnvironments(std::int64_t projectId);  // id 升序
    std::int64_t createEnvironment(std::int64_t projectId, const std::string& name,
                                   const std::string& baseUrl);
    void renameEnvironment(std::int64_t id, const std::string& name);
    void setEnvironmentBaseUrl(std::int64_t id, const std::string& baseUrl);
    void deleteEnvironment(std::int64_t id);

    // ---- requests ----
    std::vector<SavedRequest> listRequests(std::int64_t projectId);  // updated_at 降序
    std::int64_t saveRequest(const SavedRequest& r);   // id==0 插入，否则更新；返回 id
    void deleteRequest(std::int64_t id);

    // ---- global cookies ----
    std::vector<GlobalCookie> listGlobalCookies(std::int64_t projectId);
    std::int64_t saveGlobalCookie(const GlobalCookie& cookie);
    void deleteGlobalCookie(std::int64_t id, std::int64_t projectId);

    // ---- history ----
    void addHistory(const HistoryEntry& e);
    std::vector<HistoryEntry> listHistory(int limit = 50);  // 最新在前
    std::int64_t historyCount();
    std::vector<HistoryEntry> listHistoryPage(int limit, std::int64_t offset);
    void clearHistory();

    // ---- automation_tests ----
    std::vector<AutomationTest> listAutomationTests(std::int64_t projectId);
    std::int64_t saveAutomationTest(const AutomationTest& t);
    void deleteAutomationTest(std::int64_t id, std::int64_t projectId);

    // ---- load_tests ----
    void addLoadRecord(const LoadRecord& r);
    std::vector<LoadRecord> listLoadRecords(int limit = 20);  // 最新在前

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace db
