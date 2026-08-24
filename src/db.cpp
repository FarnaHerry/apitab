// db.cpp — apitab.db 实现单元（SQLiteCpp + nlohmann.json）。
// params/headers 序列化成 JSON 数组：[{"k":..,"v":..,"on":true}, ...]。
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
    created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS groups (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL,
    mode       INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS environments (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    project_id INTEGER NOT NULL DEFAULT 0,
    name       TEXT NOT NULL,
    base_url   TEXT NOT NULL DEFAULT '',
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
    body_kind  INTEGER NOT NULL DEFAULT 0,
    body       TEXT NOT NULL DEFAULT '',
    updated_at INTEGER NOT NULL DEFAULT 0
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

        // 迁移：旧库的 requests 没有 group_id / body_kind —— 补列。
        {
            bool hasGroupId = false;
            bool hasBodyKind = false;
            SQLite::Statement q(db, "PRAGMA table_info(requests)");
            while (q.executeStep()) {
                const std::string col = q.getColumn(1).getString();
                if (col == "group_id") hasGroupId = true;
                if (col == "body_kind") hasBodyKind = true;
            }
            if (!hasGroupId) {
                db.exec("ALTER TABLE requests ADD COLUMN group_id INTEGER NOT NULL DEFAULT 0");
            }
            if (!hasBodyKind) {
                db.exec("ALTER TABLE requests ADD COLUMN body_kind INTEGER NOT NULL DEFAULT 0");
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
        // 级联：先删组织下所有项目的请求，再删项目，最后删组织。
        {
            SQLite::Statement q(impl_->db,
                "DELETE FROM requests WHERE project_id IN "
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
        "SELECT id,org_id,name FROM projects WHERE org_id=? ORDER BY id ASC");
    q.bind(1, orgId);
    std::vector<db::Project> out;
    while (q.executeStep()) {
        out.push_back({q.getColumn(0).getInt64(), q.getColumn(1).getInt64(),
                       q.getColumn(2).getString()});
    }
    return out;
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
        "SELECT id,project_id,name,base_url FROM environments WHERE project_id=? ORDER BY id ASC");
    q.bind(1, projectId);
    std::vector<db::Environment> out;
    while (q.executeStep()) {
        out.push_back({q.getColumn(0).getInt64(), q.getColumn(1).getInt64(),
                       q.getColumn(2).getString(), q.getColumn(3).getString()});
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

void Db::deleteEnvironment(std::int64_t id) {
    SQLite::Statement q(impl_->db, "DELETE FROM environments WHERE id=?");
    q.bind(1, id);
    q.exec();
}

// ---- groups ----

std::vector<db::Group> Db::listGroups(std::int64_t projectId) {
    SQLite::Statement q(impl_->db,
        "SELECT id,project_id,name,mode FROM groups WHERE project_id=? ORDER BY id ASC");
    q.bind(1, projectId);
    std::vector<db::Group> out;
    while (q.executeStep()) {
        out.push_back({q.getColumn(0).getInt64(), q.getColumn(1).getInt64(),
                       q.getColumn(2).getString(),
                       static_cast<GroupMode>(q.getColumn(3).getInt())});
    }
    return out;
}

std::int64_t Db::createGroup(std::int64_t projectId, const std::string& name, GroupMode mode) {
    SQLite::Statement q(impl_->db,
        "INSERT INTO groups(project_id,name,mode,created_at) VALUES(?,?,?,?)");
    q.bind(1, projectId);
    q.bind(2, name);
    q.bind(3, static_cast<int>(mode));
    q.bind(4, static_cast<std::int64_t>(std::time(nullptr)));
    q.exec();
    return impl_->db.getLastInsertRowid();
}

void Db::renameGroup(std::int64_t id, const std::string& name) {
    SQLite::Statement q(impl_->db, "UPDATE groups SET name=? WHERE id=?");
    q.bind(1, name);
    q.bind(2, id);
    q.exec();
}

void Db::setGroupMode(std::int64_t id, GroupMode mode) {
    SQLite::Statement q(impl_->db, "UPDATE groups SET mode=? WHERE id=?");
    q.bind(1, static_cast<int>(mode));
    q.bind(2, id);
    q.exec();
}

void Db::deleteGroup(std::int64_t id) {
    impl_->db.exec("BEGIN");
    try {
        {
            SQLite::Statement q(impl_->db, "UPDATE requests SET group_id=0 WHERE group_id=?");
            q.bind(1, id);
            q.exec();
        }
        {
            SQLite::Statement q(impl_->db, "DELETE FROM groups WHERE id=?");
            q.bind(1, id);
            q.exec();
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
        "SELECT id,project_id,group_id,name,method,url,params,headers,body_kind,body,updated_at "
        "FROM requests WHERE project_id=? ORDER BY updated_at DESC");
    q.bind(1, projectId);
    std::vector<SavedRequest> out;
    while (q.executeStep()) {
        SavedRequest r;
        r.id = q.getColumn(0).getInt64();
        r.projectId = q.getColumn(1).getInt64();
        r.groupId = q.getColumn(2).getInt64();
        r.name = q.getColumn(3).getString();
        r.method = q.getColumn(4).getString();
        r.url = q.getColumn(5).getString();
        r.params = kvFromJson(q.getColumn(6).getString());
        r.headers = kvFromJson(q.getColumn(7).getString());
        r.bodyKind = static_cast<api::BodyKind>(q.getColumn(8).getInt());
        r.body = q.getColumn(9).getString();
        r.updatedAt = q.getColumn(10).getInt64();
        out.push_back(std::move(r));
    }
    return out;
}

std::int64_t Db::saveRequest(const SavedRequest& r) {
    if (r.id == 0) {
        SQLite::Statement q(impl_->db,
            "INSERT INTO requests(project_id,group_id,name,method,url,params,headers,body_kind,body,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?)");
        q.bind(1, r.projectId);
        q.bind(2, r.groupId);
        q.bind(3, r.name);
        q.bind(4, r.method);
        q.bind(5, r.url);
        q.bind(6, kvToJson(r.params));
        q.bind(7, kvToJson(r.headers));
        q.bind(8, static_cast<int>(r.bodyKind));
        q.bind(9, r.body);
        q.bind(10, r.updatedAt);
        q.exec();
        return impl_->db.getLastInsertRowid();
    }
    SQLite::Statement q(impl_->db,
        "UPDATE requests SET project_id=?,group_id=?,name=?,method=?,url=?,params=?,headers=?,body_kind=?,body=?,"
        "updated_at=? WHERE id=?");
    q.bind(1, r.projectId);
    q.bind(2, r.groupId);
    q.bind(3, r.name);
    q.bind(4, r.method);
    q.bind(5, r.url);
    q.bind(6, kvToJson(r.params));
    q.bind(7, kvToJson(r.headers));
    q.bind(8, static_cast<int>(r.bodyKind));
    q.bind(9, r.body);
    q.bind(10, r.updatedAt);
    q.bind(11, r.id);
    q.exec();
    return r.id;
}

void Db::deleteRequest(std::int64_t id) {
    SQLite::Statement q(impl_->db, "DELETE FROM requests WHERE id=?");
    q.bind(1, id);
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
