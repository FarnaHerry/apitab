// db.cpp — apitab.db 实现单元（SQLiteCpp + nlohmann.json）。
// params/headers/environments.variables 序列化成 JSON 数组：[{"k":..,"v":..,"on":true}, ...]。
module;

#include <ctime>  // std::time（created_at）
#include <SQLiteCpp/SQLiteCpp.h>

module apitab.db;

import std;
import nlohmann.json;
import apitab.api_engine;

namespace db {
namespace {

using json = nlohmann::json;

std::string kvToJson(const std::vector<api::KeyValue>& kvs) {
    json arr = json::array();
    for (const auto& kv : kvs) {
        arr.push_back({{"k", kv.key}, {"v", kv.value}, {"on", kv.enabled},
                       {"t", kv.type}, {"r", kv.remark}});
    }
    return arr.dump();
}

std::vector<api::KeyValue> kvFromJson(const std::string& text) {
    std::vector<api::KeyValue> out;
    const json arr = json::parse(text, nullptr, false);
    if (!arr.is_array()) return out;
    for (const auto& item : arr) {
        out.push_back({.key = item.value("k", ""),
                       .value = item.value("v", ""),
                       .enabled = item.value("on", true),
                       .type = item.value("t", ""),
                       .remark = item.value("r", "")});
    }
    return out;
}

std::string bodyContentsToJson(const std::array<api::BodyContent, 7>& contents) {
    json out = json::array();
    for (const auto& content : contents) {
        out.push_back({{"text", content.text}, {"fields", json::parse(kvToJson(content.fields))}});
    }
    return out.dump();
}

std::array<api::BodyContent, 7> bodyContentsFromJson(const std::string& text) {
    std::array<api::BodyContent, 7> out{};
    const json arr = json::parse(text, nullptr, false);
    if (!arr.is_array()) return out;
    for (std::size_t i = 0; i < std::min<std::size_t>(arr.size(), out.size()); ++i) {
        if (!arr[i].is_object()) continue;
        out[i].text = arr[i].value("text", "");
        if (arr[i].contains("fields")) out[i].fields = kvFromJson(arr[i]["fields"].dump());
    }
    return out;
}

// ---- 测试用例 / Mock 的 JSON 落库（requests.test_cases / requests.mock 列）----

std::string testCasesToJson(const std::vector<RequestTestCase>& cases) {
    json arr = json::array();
    for (const auto& c : cases) {
        json asserts = json::array();
        for (const auto& a : c.asserts)
            asserts.push_back({{"p", a.path}, {"e", a.equals}, {"on", a.enabled}});
        arr.push_back({{"name", c.name},
                       {"on", c.enabled},
                       {"status", c.expectStatus},
                       {"maxMs", c.maxMs},
                       {"asserts", std::move(asserts)}});
    }
    return arr.dump();
}

std::vector<RequestTestCase> testCasesFromJson(const std::string& text) {
    std::vector<RequestTestCase> out;
    const json arr = json::parse(text, nullptr, false);
    if (!arr.is_array()) return out;
    for (const auto& item : arr) {
        if (!item.is_object()) continue;
        RequestTestCase c;
        c.name = item.value("name", "");
        c.enabled = item.value("on", true);
        c.expectStatus = item.value("status", 0);
        c.maxMs = item.value("maxMs", 0.0);
        if (item.contains("asserts") && item["asserts"].is_array()) {
            for (const auto& a : item["asserts"]) {
                if (!a.is_object()) continue;
                c.asserts.push_back({.path = a.value("p", ""),
                                     .equals = a.value("e", ""),
                                     .enabled = a.value("on", true)});
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::string mockToJson(const RequestMock& m) {
    return json({{"on", m.enabled},
                 {"status", m.status},
                 {"headers", json::parse(kvToJson(m.headers))},
                 {"body", m.body},
                 {"delayMs", m.delayMs}})
        .dump();
}

RequestMock mockFromJson(const std::string& text) {
    RequestMock m;
    const json obj = json::parse(text, nullptr, false);
    if (!obj.is_object()) return m;
    m.enabled = obj.value("on", false);
    m.status = obj.value("status", 200);
    if (obj.contains("headers")) m.headers = kvFromJson(obj["headers"].dump());
    m.body = obj.value("body", "");
    m.delayMs = obj.value("delayMs", 0);
    return m;
}

} // namespace

const char* groupModeName(GroupMode mode) { return mode == GroupMode::Path ? "路径" : "仅名称"; }

std::string groupPath(const std::vector<std::string>& segs) {
    std::string out;
    for (const auto& s : segs) {
        if (!out.empty()) out += '/';
        out += s;
    }
    return out;
}

struct Db::Impl {
    SQLite::Database db;

    explicit Impl(const std::filesystem::path& file)
        : db(file.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
        db.exec("PRAGMA journal_mode=WAL;");
        db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS organizations (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS projects (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    org_id     INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    headers     TEXT NOT NULL DEFAULT '[]',
    created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS groups (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL DEFAULT 0,
    parent_id  INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL,
    mode       INTEGER NOT NULL DEFAULT 0,
    path       TEXT NOT NULL DEFAULT '',
    created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS environments (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL,
    base_url   TEXT NOT NULL DEFAULT '',
    variables  TEXT NOT NULL DEFAULT '[]',
    created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS requests (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL DEFAULT 0,
    group_id   INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL,
    method     TEXT NOT NULL DEFAULT 'GET',
    url        TEXT NOT NULL DEFAULT '',
    params     TEXT NOT NULL DEFAULT '[]',
    headers    TEXT NOT NULL DEFAULT '[]',
    body_kind    INTEGER NOT NULL DEFAULT 0,
    body         TEXT NOT NULL DEFAULT '',
    request_kind INTEGER NOT NULL DEFAULT 0,
    ws_protocol  TEXT NOT NULL DEFAULT '',
    cookies      TEXT NOT NULL DEFAULT '[]',
    follow_redirects INTEGER NOT NULL DEFAULT 1,
    allow_json_comments INTEGER NOT NULL DEFAULT 1,
    body_contents TEXT NOT NULL DEFAULT '{}',
    test_cases    TEXT NOT NULL DEFAULT '[]',
    mock          TEXT NOT NULL DEFAULT '{}',
    updated_at   INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS global_cookies (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    value TEXT NOT NULL DEFAULT '',
    enabled INTEGER NOT NULL DEFAULT 1
);
CREATE TABLE IF NOT EXISTS history (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    request_id  INTEGER NOT NULL DEFAULT 0,
    method      TEXT NOT NULL,
    url         TEXT NOT NULL,
    status      INTEGER NOT NULL DEFAULT 0,
    duration_ms REAL NOT NULL DEFAULT 0,
    size_bytes  INTEGER NOT NULL DEFAULT 0,
    error       TEXT NOT NULL DEFAULT '',
    created_at  INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS automation_tests (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL DEFAULT 0,
    name TEXT NOT NULL DEFAULT '',
    method TEXT NOT NULL DEFAULT 'GET',
    url TEXT NOT NULL DEFAULT '',
    params TEXT NOT NULL DEFAULT '[]', headers TEXT NOT NULL DEFAULT '[]', cookies TEXT NOT NULL DEFAULT '[]',
    body_kind INTEGER NOT NULL DEFAULT 0, body TEXT NOT NULL DEFAULT '', body_contents TEXT NOT NULL DEFAULT '{}',
    follow_redirects INTEGER NOT NULL DEFAULT 1, allow_json_comments INTEGER NOT NULL DEFAULT 1,
    vus INTEGER NOT NULL DEFAULT 10, duration TEXT NOT NULL DEFAULT '30s', updated_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS load_tests (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    request_id INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL DEFAULT '',
    url        TEXT NOT NULL,
    vus        INTEGER NOT NULL DEFAULT 0,
    duration   TEXT NOT NULL DEFAULT '',
    requests   INTEGER NOT NULL DEFAULT 0,
    rps        REAL NOT NULL DEFAULT 0,
    p50_ms     REAL NOT NULL DEFAULT 0,
    p90_ms     REAL NOT NULL DEFAULT 0,
    p95_ms     REAL NOT NULL DEFAULT 0,
    p99_ms     REAL NOT NULL DEFAULT 0,
    fail_rate  REAL NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT 0
);
)SQL");

        // 迁移：旧库的 requests 没有 group_id / body_kind，groups 没有 parent_id，
        // environments 没有 variables。
        {
            bool hasGroupId = false;
            bool hasBodyKind = false;
            bool hasRequestKind = false;
            bool hasWsProtocol = false;
            bool hasCookies = false;
            bool hasFollowRedirects = false;
            bool hasAllowJsonComments = false;
            bool hasBodyContents = false;
            bool hasTestCases = false;
            bool hasMock = false;
            bool hasGlobalCookieProjectId = false;
            bool hasParentId = false;
            bool hasEnvVariables = false;
            SQLite::Statement cookieInfo(db, "PRAGMA table_info(global_cookies)");
            while (cookieInfo.executeStep()) {
                if (cookieInfo.getColumn(1).getString() == "project_id") hasGlobalCookieProjectId = true;
            }
            if (!hasGlobalCookieProjectId) {
                db.exec("ALTER TABLE global_cookies ADD COLUMN project_id INTEGER NOT NULL DEFAULT 0");
            }

            SQLite::Statement q(db, "PRAGMA table_info(requests)");
            while (q.executeStep()) {
                const std::string col = q.getColumn(1).getString();
                if (col == "group_id") hasGroupId = true;
                if (col == "body_kind") hasBodyKind = true;
                if (col == "request_kind") hasRequestKind = true;
                if (col == "ws_protocol") hasWsProtocol = true;
                if (col == "cookies") hasCookies = true;
                if (col == "follow_redirects") hasFollowRedirects = true;
                if (col == "allow_json_comments") hasAllowJsonComments = true;
                if (col == "body_contents") hasBodyContents = true;
                if (col == "test_cases") hasTestCases = true;
                if (col == "mock") hasMock = true;
            }
            bool hasGroupPath = false;
            SQLite::Statement groups(db, "PRAGMA table_info(groups)");
            while (groups.executeStep()) {
                if (groups.getColumn(1).getString() == "parent_id") hasParentId = true;
                if (groups.getColumn(1).getString() == "path") hasGroupPath = true;
            }
            if (!hasGroupPath) {
                db.exec("ALTER TABLE groups ADD COLUMN path TEXT NOT NULL DEFAULT ''");
            }
            SQLite::Statement envs(db, "PRAGMA table_info(environments)");
            while (envs.executeStep()) {
                if (envs.getColumn(1).getString() == "variables") hasEnvVariables = true;
            }
            bool hasProjectDescription = false;
            bool hasProjectHeaders = false;
            SQLite::Statement projs(db, "PRAGMA table_info(projects)");
            while (projs.executeStep()) {
                if (projs.getColumn(1).getString() == "description") hasProjectDescription = true;
                if (projs.getColumn(1).getString() == "headers") hasProjectHeaders = true;
            }
            if (!hasProjectDescription) {
                db.exec("ALTER TABLE projects ADD COLUMN description TEXT NOT NULL DEFAULT ''");
            }
            if (!hasProjectHeaders) {
                db.exec("ALTER TABLE projects ADD COLUMN headers TEXT NOT NULL DEFAULT '[]'");
            }
            if (!hasGroupId) {
                db.exec("ALTER TABLE requests ADD COLUMN group_id INTEGER NOT NULL DEFAULT 0");
            }
            if (!hasBodyKind) {
                db.exec("ALTER TABLE requests ADD COLUMN body_kind INTEGER NOT NULL DEFAULT 0");
            }
            if (!hasRequestKind) {
                db.exec("ALTER TABLE requests ADD COLUMN request_kind INTEGER NOT NULL DEFAULT 0");
            }
            if (!hasWsProtocol) {
                db.exec("ALTER TABLE requests ADD COLUMN ws_protocol TEXT NOT NULL DEFAULT ''");
            }
            if (!hasCookies) {
                db.exec("ALTER TABLE requests ADD COLUMN cookies TEXT NOT NULL DEFAULT '[]'");
            }
            if (!hasFollowRedirects) {
                db.exec("ALTER TABLE requests ADD COLUMN follow_redirects INTEGER NOT NULL DEFAULT 1");
            }
            if (!hasAllowJsonComments) {
                db.exec("ALTER TABLE requests ADD COLUMN allow_json_comments INTEGER NOT NULL DEFAULT 1");
            }
            if (!hasBodyContents) {
                db.exec("ALTER TABLE requests ADD COLUMN body_contents TEXT NOT NULL DEFAULT '{}'");
            }
            if (!hasTestCases) {
                db.exec("ALTER TABLE requests ADD COLUMN test_cases TEXT NOT NULL DEFAULT '[]'");
            }
            if (!hasMock) {
                db.exec("ALTER TABLE requests ADD COLUMN mock TEXT NOT NULL DEFAULT '{}'");
            }
            if (!hasParentId) {
                db.exec("ALTER TABLE groups ADD COLUMN parent_id INTEGER NOT NULL DEFAULT 0");
            }
            if (!hasEnvVariables) {
                db.exec("ALTER TABLE environments ADD COLUMN variables TEXT NOT NULL DEFAULT '[]'");
            }
        }
    }
};

Db::Db(const std::filesystem::path& file) : impl_(std::make_unique<Impl>(file)) {}
Db::~Db() = default;

// ---- organizations / projects ----

std::vector<db::Org> Db::listOrgs() {
    SQLite::Statement q(impl_->db, "SELECT id,name FROM organizations ORDER BY id ASC");
    std::vector<db::Org> out;
    while (q.executeStep()) {
        out.push_back({q.getColumn(0).getInt64(), q.getColumn(1).getString()});
    }
    return out;
}

std::int64_t Db::createOrg(const std::string& name) {
    SQLite::Statement q(impl_->db, "INSERT INTO organizations(name,created_at) VALUES(?,?)");
    q.bind(1, name);
    q.bind(2, static_cast<std::int64_t>(std::time(nullptr)));
    q.exec();
    return impl_->db.getLastInsertRowid();
}

void Db::renameOrg(std::int64_t id, const std::string& name) {
    SQLite::Statement q(impl_->db, "UPDATE organizations SET name=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, id);
    q.exec();
}

void Db::deleteOrg(std::int64_t id) {
    impl_->db.exec("BEGIN");
    try {
        // 级联：先删组织下所有项目的请求、分组、环境，再删项目和组织。
        {
            SQLite::Statement q(impl_->db,
                "DELETE FROM requests WHERE project_id IN "
                "(SELECT id FROM projects WHERE org_id=?)");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db,
                "DELETE FROM groups WHERE project_id IN "
                "(SELECT id FROM projects WHERE org_id=?)");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db,
                "DELETE FROM environments WHERE project_id IN "
                "(SELECT id FROM projects WHERE org_id=?)");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db, "DELETE FROM projects WHERE org_id=?");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db, "DELETE FROM organizations WHERE id=?");
            q.bind(1, id);
            q.exec();
        }
        impl_->db.exec("COMMIT");
    } catch (...) {
        impl_->db.exec("ROLLBACK");
        throw;
    }
}

std::vector<db::Project> Db::listProjects(std::int64_t orgId) {
    SQLite::Statement q(impl_->db,
        "SELECT id,org_id,name,description,headers FROM projects WHERE org_id=? ORDER BY id ASC");
    q.bind(1, orgId);
    std::vector<db::Project> out;
    while (q.executeStep()) {
        db::Project p;
        p.id = q.getColumn(0).getInt64();
        p.orgId = q.getColumn(1).getInt64();
        p.name = q.getColumn(2).getString();
        p.description = q.getColumn(3).getString();
        p.headers = kvFromJson(q.getColumn(4).getString());
        out.push_back(std::move(p));
    }
    return out;
}

void Db::updateProjectMeta(std::int64_t id, const std::string& name, const std::string& description,
                           const std::vector<api::KeyValue>& headers) {
    SQLite::Statement q(impl_->db, "UPDATE projects SET name=?,description=?,headers=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, description);
    q.bind(3, kvToJson(headers));
    q.bind(4, id);
    q.exec();
}

std::int64_t Db::createProject(std::int64_t orgId, const std::string& name) {
    SQLite::Statement q(impl_->db, "INSERT INTO projects(org_id,name,created_at) VALUES(?,?,?)");
    q.bind(1, orgId);
    q.bind(2, name);
    q.bind(3, static_cast<std::int64_t>(std::time(nullptr)));
    q.exec();
    return impl_->db.getLastInsertRowid();
}

void Db::renameProject(std::int64_t id, const std::string& name) {
    SQLite::Statement q(impl_->db, "UPDATE projects SET name=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, id);
    q.exec();
}

void Db::deleteProject(std::int64_t id) {
    impl_->db.exec("BEGIN");
    try {
        {
            SQLite::Statement q(impl_->db, "DELETE FROM requests WHERE project_id=?");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db, "DELETE FROM groups WHERE project_id=?");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db, "DELETE FROM environments WHERE project_id=?");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db, "DELETE FROM projects WHERE id=?");
            q.bind(1, id);
            q.exec();
        }
        impl_->db.exec("COMMIT");
    } catch (...) {
        impl_->db.exec("ROLLBACK");
        throw;
    }
}

std::int64_t Db::ensureDefaultProject() {
    // 任一项目 → 用之；否则建默认组织 + 默认项目，并把游离请求（project_id=0
    // 或指向已删项目）归入默认项目。
    {
        SQLite::Statement q(impl_->db, "SELECT id FROM projects ORDER BY id ASC LIMIT 1");
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
    }
    const std::int64_t orgId = createOrg("默认组织");
    const std::int64_t projectId = createProject(orgId, "默认项目");
    createEnvironment(projectId, "localhost", "http://localhost");
    {
        SQLite::Statement q(impl_->db,
            "UPDATE requests SET project_id=? WHERE project_id=0 OR project_id NOT IN "
            "(SELECT id FROM projects)");
        q.bind(1, projectId);
        q.exec();
    }
    return projectId;
}

// ---- environments ----

std::vector<db::Environment> Db::listEnvironments(std::int64_t projectId) {
    SQLite::Statement q(impl_->db,
        "SELECT id,project_id,name,base_url,variables FROM environments WHERE project_id=? ORDER BY id ASC");
    q.bind(1, projectId);
    std::vector<db::Environment> out;
    while (q.executeStep()) {
        out.push_back({q.getColumn(0).getInt64(), q.getColumn(1).getInt64(),
                       q.getColumn(2).getString(), q.getColumn(3).getString(),
                       kvFromJson(q.getColumn(4).getString())});
    }
    return out;
}

std::int64_t Db::createEnvironment(std::int64_t projectId, const std::string& name,
                                   const std::string& baseUrl) {
    SQLite::Statement q(impl_->db,
        "INSERT INTO environments(project_id,name,base_url,created_at) VALUES(?,?,?,?)");
    q.bind(1, projectId);
    q.bind(2, name);
    q.bind(3, baseUrl);
    q.bind(4, static_cast<std::int64_t>(std::time(nullptr)));
    q.exec();
    return impl_->db.getLastInsertRowid();
}

void Db::renameEnvironment(std::int64_t id, const std::string& name) {
    SQLite::Statement q(impl_->db, "UPDATE environments SET name=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, id);
    q.exec();
}

void Db::setEnvironmentBaseUrl(std::int64_t id, const std::string& baseUrl) {
    SQLite::Statement q(impl_->db, "UPDATE environments SET base_url=? WHERE id=?");
    q.bind(1, baseUrl);
    q.bind(2, id);
    q.exec();
}

// variables 与 params/headers 同模式：KV 数组序列化成 JSON 存 TEXT 列。
void Db::setEnvironmentVariables(std::int64_t id, const std::vector<api::KeyValue>& vars) {
    SQLite::Statement q(impl_->db, "UPDATE environments SET variables=? WHERE id=?");
    q.bind(1, kvToJson(vars));
    q.bind(2, id);
    q.exec();
}

void Db::deleteEnvironment(std::int64_t id) {
    SQLite::Statement q(impl_->db, "DELETE FROM environments WHERE id=?");
    q.bind(1, id);
    q.exec();
}

// ---- groups ----

std::vector<db::Group> Db::listGroups(std::int64_t projectId) {
    SQLite::Statement q(impl_->db,
        "SELECT id,project_id,parent_id,name,mode,path FROM groups WHERE project_id=? ORDER BY id ASC");
    q.bind(1, projectId);
    std::vector<db::Group> out;
    while (q.executeStep()) {
        out.push_back({.id = q.getColumn(0).getInt64(),
                       .projectId = q.getColumn(1).getInt64(),
                       .parentId = q.getColumn(2).getInt64(),
                       .name = q.getColumn(3).getString(),
                       .mode = static_cast<GroupMode>(q.getColumn(4).getInt()),
                       .path = q.getColumn(5).getString()});
    }
    return out;
}

std::int64_t Db::createGroup(std::int64_t projectId, const std::string& name, GroupMode mode,
                             std::int64_t parentId, const std::string& path) {
    SQLite::Statement q(impl_->db,
        "INSERT INTO groups(project_id,parent_id,name,mode,path,created_at) VALUES(?,?,?,?,?,?)");
    q.bind(1, projectId);
    q.bind(2, parentId);
    q.bind(3, name);
    q.bind(4, static_cast<int>(mode));
    q.bind(5, path);
    q.bind(6, static_cast<std::int64_t>(std::time(nullptr)));
    q.exec();
    return impl_->db.getLastInsertRowid();
}

void Db::renameGroup(std::int64_t id, const std::string& name) {
    SQLite::Statement q(impl_->db, "UPDATE groups SET name=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, id);
    q.exec();
}

void Db::updateGroup(std::int64_t id, const std::string& name, GroupMode mode,
                     const std::string& path) {
    SQLite::Statement q(impl_->db, "UPDATE groups SET name=?,mode=?,path=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, static_cast<int>(mode));
    q.bind(3, path);
    q.bind(4, id);
    q.exec();
}

void Db::setGroupMode(std::int64_t id, GroupMode mode) {
    SQLite::Statement q(impl_->db, "UPDATE groups SET mode=? WHERE id=?");
    q.bind(1, static_cast<int>(mode));
    q.bind(2, id);
    q.exec();
}

void Db::setGroupParent(std::int64_t id, std::int64_t parentId) {
    SQLite::Statement q(impl_->db, "UPDATE groups SET parent_id=? WHERE id=?");
    q.bind(1, parentId);
    q.bind(2, id);
    q.exec();
}

void Db::deleteGroup(std::int64_t id) {
    impl_->db.exec("BEGIN");
    try {
        std::vector<std::int64_t> ids{id};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            SQLite::Statement children(impl_->db, "SELECT id FROM groups WHERE parent_id=?");
            children.bind(1, ids[i]);
            while (children.executeStep()) ids.push_back(children.getColumn(0).getInt64());
        }
        for (const std::int64_t groupId : ids) {
            SQLite::Statement requests(impl_->db, "UPDATE requests SET group_id=0 WHERE group_id=?");
            requests.bind(1, groupId);
            requests.exec();
        }
        for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
            SQLite::Statement group(impl_->db, "DELETE FROM groups WHERE id=?");
            group.bind(1, *it);
            group.exec();
        }
        impl_->db.exec("COMMIT");
    } catch (...) {
        impl_->db.exec("ROLLBACK");
        throw;
    }
}

// ---- requests ----

std::vector<SavedRequest> Db::listRequests(std::int64_t projectId) {
    SQLite::Statement q(impl_->db,
        "SELECT id,project_id,group_id,name,request_kind,ws_protocol,cookies,follow_redirects,allow_json_comments,method,url,params,headers,body_kind,body,body_contents,test_cases,mock,updated_at "
        "FROM requests WHERE project_id=? ORDER BY updated_at DESC");
    q.bind(1, projectId);
    std::vector<SavedRequest> out;
    while (q.executeStep()) {
        // 旧库迁移字段使用默认值：follow redirects / JSON comments 均开启。
        SavedRequest r;
        r.id = q.getColumn(0).getInt64();
        r.projectId = q.getColumn(1).getInt64();
        r.groupId = q.getColumn(2).getInt64();
        r.name = q.getColumn(3).getString();
        r.kind = static_cast<api::RequestKind>(q.getColumn(4).getInt());
        r.wsProtocol = q.getColumn(5).getString();
        r.cookies = kvFromJson(q.getColumn(6).getString());
        r.followRedirects = q.getColumn(7).getInt() != 0;
        r.allowJsonComments = q.getColumn(8).getInt() != 0;
        r.method = q.getColumn(9).getString();
        r.url = q.getColumn(10).getString();
        r.params = kvFromJson(q.getColumn(11).getString());
        r.headers = kvFromJson(q.getColumn(12).getString());
        r.bodyKind = static_cast<api::BodyKind>(q.getColumn(13).getInt());
        r.body = q.getColumn(14).getString();
        r.bodyContents = bodyContentsFromJson(q.getColumn(15).getString());
        const auto bodyIndex = static_cast<std::size_t>(r.bodyKind);
        if (bodyIndex < r.bodyContents.size() && r.bodyContents[bodyIndex].text.empty()) {
            r.bodyContents[bodyIndex].text = r.body;
        }
        r.testCases = testCasesFromJson(q.getColumn(16).getString());
        r.mock = mockFromJson(q.getColumn(17).getString());
        r.updatedAt = q.getColumn(18).getInt64();
        out.push_back(std::move(r));
    }
    return out;
}

std::int64_t Db::saveRequest(const SavedRequest& r) {
    if (r.id == 0) {
        SQLite::Statement q(impl_->db,
            "INSERT INTO requests(project_id,group_id,name,request_kind,ws_protocol,cookies,follow_redirects,allow_json_comments,method,url,params,headers,body_kind,body,body_contents,test_cases,mock,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        q.bind(1, r.projectId);
        q.bind(2, r.groupId);
        q.bind(3, r.name);
        q.bind(4, static_cast<int>(r.kind));
        q.bind(5, r.wsProtocol);
        q.bind(6, kvToJson(r.cookies));
        q.bind(7, r.followRedirects ? 1 : 0);
        q.bind(8, r.allowJsonComments ? 1 : 0);
        q.bind(9, r.method);
        q.bind(10, r.url);
        q.bind(11, kvToJson(r.params));
        q.bind(12, kvToJson(r.headers));
        q.bind(13, static_cast<int>(r.bodyKind));
        q.bind(14, r.body);
        q.bind(15, bodyContentsToJson(r.bodyContents));
        q.bind(16, testCasesToJson(r.testCases));
        q.bind(17, mockToJson(r.mock));
        q.bind(18, r.updatedAt);
        q.exec();
        return impl_->db.getLastInsertRowid();
    }
    SQLite::Statement q(impl_->db,
        "UPDATE requests SET project_id=?,group_id=?,name=?,request_kind=?,ws_protocol=?,cookies=?,follow_redirects=?,allow_json_comments=?,method=?,url=?,params=?,headers=?,body_kind=?,body=?,body_contents=?,test_cases=?,mock=?,"
        "updated_at=? WHERE id=?");
    q.bind(1, r.projectId);
    q.bind(2, r.groupId);
    q.bind(3, r.name);
    q.bind(4, static_cast<int>(r.kind));
    q.bind(5, r.wsProtocol);
    q.bind(6, kvToJson(r.cookies));
    q.bind(7, r.followRedirects ? 1 : 0);
    q.bind(8, r.allowJsonComments ? 1 : 0);
    q.bind(9, r.method);
    q.bind(10, r.url);
    q.bind(11, kvToJson(r.params));
    q.bind(12, kvToJson(r.headers));
    q.bind(13, static_cast<int>(r.bodyKind));
    q.bind(14, r.body);
    q.bind(15, bodyContentsToJson(r.bodyContents));
    q.bind(16, testCasesToJson(r.testCases));
    q.bind(17, mockToJson(r.mock));
    q.bind(18, r.updatedAt);
    q.bind(19, r.id);
    q.exec();
    return r.id;
}

void Db::deleteRequest(std::int64_t id) {
    SQLite::Statement q(impl_->db, "DELETE FROM requests WHERE id=?");
    q.bind(1, id);
    q.exec();
}

// ---- global cookies ----

std::vector<GlobalCookie> Db::listGlobalCookies(std::int64_t projectId) {
    SQLite::Statement q(impl_->db, "SELECT id,project_id,name,value,enabled FROM global_cookies WHERE project_id=? ORDER BY id ASC");
    q.bind(1, projectId);
    std::vector<GlobalCookie> out;
    while (q.executeStep()) {
        out.push_back({q.getColumn(0).getInt64(), q.getColumn(1).getInt64(), q.getColumn(2).getString(),
                       q.getColumn(3).getString(), q.getColumn(4).getInt() != 0});
    }
    return out;
}

std::int64_t Db::saveGlobalCookie(const GlobalCookie& cookie) {
    if (cookie.id == 0) {
        SQLite::Statement q(impl_->db, "INSERT INTO global_cookies(project_id,name,value,enabled) VALUES(?,?,?,?)");
        q.bind(1, cookie.projectId);
        q.bind(2, cookie.name);
        q.bind(3, cookie.value);
        q.bind(4, cookie.enabled ? 1 : 0);
        q.exec();
        return impl_->db.getLastInsertRowid();
    }
    SQLite::Statement q(impl_->db, "UPDATE global_cookies SET name=?,value=?,enabled=? WHERE id=? AND project_id=?");
    q.bind(1, cookie.name);
    q.bind(2, cookie.value);
    q.bind(3, cookie.enabled ? 1 : 0);
    q.bind(4, cookie.id);
    q.bind(5, cookie.projectId);
    q.exec();
    return cookie.id;
}

void Db::deleteGlobalCookie(std::int64_t id, std::int64_t projectId) {
    SQLite::Statement q(impl_->db, "DELETE FROM global_cookies WHERE id=? AND project_id=?");
    q.bind(1, id);
    q.bind(2, projectId);
    q.exec();
}

// ---- history ----

void Db::addHistory(const HistoryEntry& e) {
    SQLite::Statement q(impl_->db,
        "INSERT INTO history(request_id,method,url,status,duration_ms,size_bytes,error,created_at) "
        "VALUES(?,?,?,?,?,?,?,?)");
    q.bind(1, e.requestId);
    q.bind(2, e.method);
    q.bind(3, e.url);
    q.bind(4, static_cast<std::int64_t>(e.status));
    q.bind(5, e.durationMs);
    q.bind(6, e.sizeBytes);
    q.bind(7, e.error);
    q.bind(8, e.createdAt);
    q.exec();
}

std::vector<HistoryEntry> Db::listHistory(int limit) {
    return listHistoryPage(limit, 0);
}

std::int64_t Db::historyCount() {
    SQLite::Statement q(impl_->db, "SELECT COUNT(*) FROM history");
    return q.executeStep() ? q.getColumn(0).getInt64() : 0;
}

std::vector<HistoryEntry> Db::listHistoryPage(int limit, std::int64_t offset) {
    const int safeLimit = std::max(1, limit);
    const std::int64_t safeOffset = std::max<std::int64_t>(0, offset);
    SQLite::Statement q(impl_->db,
        "SELECT id,request_id,method,url,status,duration_ms,size_bytes,error,created_at "
        "FROM history ORDER BY id DESC LIMIT ? OFFSET ?");
    q.bind(1, safeLimit);
    q.bind(2, safeOffset);
    std::vector<HistoryEntry> out;
    while (q.executeStep()) {
        HistoryEntry e;
        e.id = q.getColumn(0).getInt64();
        e.requestId = q.getColumn(1).getInt64();
        e.method = q.getColumn(2).getString();
        e.url = q.getColumn(3).getString();
        e.status = static_cast<long>(q.getColumn(4).getInt64());
        e.durationMs = q.getColumn(5).getDouble();
        e.sizeBytes = q.getColumn(6).getInt64();
        e.error = q.getColumn(7).getString();
        e.createdAt = q.getColumn(8).getInt64();
        out.push_back(std::move(e));
    }
    return out;
}

void Db::clearHistory() {
    impl_->db.exec("DELETE FROM history");
}

// ---- automation_tests ----
std::vector<AutomationTest> Db::listAutomationTests(std::int64_t projectId) {
    SQLite::Statement q(impl_->db, "SELECT id,project_id,name,method,url,params,headers,cookies,body_kind,body,body_contents,follow_redirects,allow_json_comments,vus,duration,updated_at FROM automation_tests WHERE project_id=? ORDER BY id ASC");
    q.bind(1, projectId); std::vector<AutomationTest> out;
    while (q.executeStep()) { AutomationTest t; t.id=q.getColumn(0).getInt64(); t.projectId=q.getColumn(1).getInt64(); t.name=q.getColumn(2).getString(); t.method=q.getColumn(3).getString(); t.url=q.getColumn(4).getString(); t.params=kvFromJson(q.getColumn(5).getString()); t.headers=kvFromJson(q.getColumn(6).getString()); t.cookies=kvFromJson(q.getColumn(7).getString()); t.bodyKind=static_cast<api::BodyKind>(q.getColumn(8).getInt()); t.body=q.getColumn(9).getString(); t.bodyContents=bodyContentsFromJson(q.getColumn(10).getString()); t.followRedirects=q.getColumn(11).getInt()!=0; t.allowJsonComments=q.getColumn(12).getInt()!=0; t.vus=q.getColumn(13).getInt(); t.duration=q.getColumn(14).getString(); t.updatedAt=q.getColumn(15).getInt64(); out.push_back(std::move(t)); }
    return out;
}
std::int64_t Db::saveAutomationTest(const AutomationTest& t) {
    if (t.id == 0) { SQLite::Statement q(impl_->db, "INSERT INTO automation_tests(project_id,name,method,url,params,headers,cookies,body_kind,body,body_contents,follow_redirects,allow_json_comments,vus,duration,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"); q.bind(1,t.projectId); q.bind(2,t.name); q.bind(3,t.method); q.bind(4,t.url); q.bind(5,kvToJson(t.params)); q.bind(6,kvToJson(t.headers)); q.bind(7,kvToJson(t.cookies)); q.bind(8,static_cast<int>(t.bodyKind)); q.bind(9,t.body); q.bind(10,bodyContentsToJson(t.bodyContents)); q.bind(11,t.followRedirects?1:0); q.bind(12,t.allowJsonComments?1:0); q.bind(13,t.vus); q.bind(14,t.duration); q.bind(15,t.updatedAt); q.exec(); return impl_->db.getLastInsertRowid(); }
    SQLite::Statement q(impl_->db, "UPDATE automation_tests SET name=?,method=?,url=?,params=?,headers=?,cookies=?,body_kind=?,body=?,body_contents=?,follow_redirects=?,allow_json_comments=?,vus=?,duration=?,updated_at=? WHERE id=? AND project_id=?"); q.bind(1,t.name); q.bind(2,t.method); q.bind(3,t.url); q.bind(4,kvToJson(t.params)); q.bind(5,kvToJson(t.headers)); q.bind(6,kvToJson(t.cookies)); q.bind(7,static_cast<int>(t.bodyKind)); q.bind(8,t.body); q.bind(9,bodyContentsToJson(t.bodyContents)); q.bind(10,t.followRedirects?1:0); q.bind(11,t.allowJsonComments?1:0); q.bind(12,t.vus); q.bind(13,t.duration); q.bind(14,t.updatedAt); q.bind(15,t.id); q.bind(16,t.projectId); q.exec(); return t.id;
}
void Db::deleteAutomationTest(std::int64_t id, std::int64_t projectId) { SQLite::Statement q(impl_->db,"DELETE FROM automation_tests WHERE id=? AND project_id=?"); q.bind(1,id); q.bind(2,projectId); q.exec(); }

// ---- load_tests ----

void Db::addLoadRecord(const LoadRecord& r) {
    SQLite::Statement q(impl_->db,
        "INSERT INTO load_tests(request_id,name,url,vus,duration,requests,rps,"
        "p50_ms,p90_ms,p95_ms,p99_ms,fail_rate,created_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    q.bind(1, r.requestId);
    q.bind(2, r.name);
    q.bind(3, r.url);
    q.bind(4, r.vus);
    q.bind(5, r.duration);
    q.bind(6, r.requests);
    q.bind(7, r.rps);
    q.bind(8, r.p50Ms);
    q.bind(9, r.p90Ms);
    q.bind(10, r.p95Ms);
    q.bind(11, r.p99Ms);
    q.bind(12, r.failRate);
    q.bind(13, r.createdAt);
    q.exec();
}

std::vector<LoadRecord> Db::listLoadRecords(int limit) {
    SQLite::Statement q(impl_->db,
        "SELECT id,request_id,name,url,vus,duration,requests,rps,"
        "p50_ms,p90_ms,p95_ms,p99_ms,fail_rate,created_at "
        "FROM load_tests ORDER BY id DESC LIMIT ?");
    q.bind(1, limit);
    std::vector<LoadRecord> out;
    while (q.executeStep()) {
        LoadRecord r;
        r.id = q.getColumn(0).getInt64();
        r.requestId = q.getColumn(1).getInt64();
        r.name = q.getColumn(2).getString();
        r.url = q.getColumn(3).getString();
        r.vus = q.getColumn(4).getInt();
        r.duration = q.getColumn(5).getString();
        r.requests = q.getColumn(6).getInt64();
        r.rps = q.getColumn(7).getDouble();
        r.p50Ms = q.getColumn(8).getDouble();
        r.p90Ms = q.getColumn(9).getDouble();
        r.p95Ms = q.getColumn(10).getDouble();
        r.p99Ms = q.getColumn(11).getDouble();
        r.failRate = q.getColumn(12).getDouble();
        r.createdAt = q.getColumn(13).getInt64();
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace db
