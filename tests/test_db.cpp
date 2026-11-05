// test_db.cpp — apitab.db 的持久化契约测试。
//
// 只覆盖本轮新增的 requests.doc 字段链路，不重复 store/UI 行为：
//   1) 新建库保存带 doc 的请求，listRequests 原样读回；
//   2) 旧库（requests 无 doc 列）打开后自动补列，既有行 doc 为空串；
//   3) UPDATE 路径同样写回 doc（保存两次、第二次改内容）。
//
// 这是普通 TU（不 import std）：SQLiteCpp 只在造"旧库"夹具与检查列时使用，
// 按项目约定显式 include 所用标准设施。

#include <SQLiteCpp/SQLiteCpp.h>

#include <exception>
#include <filesystem>
#include <format>
#include <print>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define APITAB_TEST_GETPID _getpid
#else
#include <unistd.h>
#define APITAB_TEST_GETPID getpid
#endif

import apitab.api_engine;
import apitab.db;

namespace {

int failures = 0;

void check(bool cond, std::string_view what) {
    if (cond) return;
    std::println("FAIL: {}", what);
    ++failures;
}

// 每个用例独立临时目录；析构顺序保证先删库文件再删目录。
class TempDir {
public:
    TempDir() {
        const auto base = std::filesystem::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            auto candidate =
                base / std::format("apitab-db-test-{}-{}", APITAB_TEST_GETPID(), i);
            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        throw std::runtime_error("无法创建临时测试目录");
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

db::SavedRequest MakeRequest(std::int64_t projectId, std::string name, std::string doc) {
    db::SavedRequest r;
    r.projectId = projectId;
    r.name = std::move(name);
    r.method = "POST";
    r.url = "https://example.com/items";
    r.doc = std::move(doc);
    return r;
}

const db::SavedRequest* FindByName(const std::vector<db::SavedRequest>& rows,
                                   std::string_view name) {
    for (const db::SavedRequest& r : rows)
        if (r.name == name) return &r;
    return nullptr;
}

void TestRoundTrip() {
    TempDir dir;
    db::Db handle(dir.path() / "roundtrip.db");
    const std::int64_t project = handle.ensureDefaultProject();

    const std::string doc =
        "# 创建条目\n\n- 需要 **名称**\n- 返回 201\n\n```json\n{\"id\": 1}\n```\n";
    db::SavedRequest saved = MakeRequest(project, "新建条目", doc);
    saved.updatedAt = 1700000000;
    const std::int64_t id = handle.saveRequest(saved);
    check(id > 0, "saveRequest 返回新 id");

    const auto rows = handle.listRequests(project);
    const db::SavedRequest* found = FindByName(rows, "新建条目");
    check(found != nullptr, "listRequests 能读到刚保存的请求");
    if (found) {
        check(found->doc == doc, "doc 原样读回（含换行与代码块）");
        check(found->url == "https://example.com/items", "其它字段不受 doc 列影响");
    }
}

void TestUpdateKeepsDoc() {
    TempDir dir;
    db::Db handle(dir.path() / "update.db");
    const std::int64_t project = handle.ensureDefaultProject();

    db::SavedRequest saved = MakeRequest(project, "更新文档", "第一版");
    saved.updatedAt = 1700000001;
    saved.id = handle.saveRequest(saved);

    saved.name = "更新文档（改名）";
    saved.doc = "第二版：**重点**";
    saved.updatedAt = 1700000002;
    handle.saveRequest(saved);

    const auto rows = handle.listRequests(project);
    check(rows.size() == 1, "UPDATE 不应插入新行");
    if (!rows.empty()) {
        check(rows.front().doc == "第二版：**重点**", "UPDATE 写回新 doc");
        check(rows.front().name == "更新文档（改名）", "UPDATE 写回新名称");
    }
}

// 旧库：requests 表没有 doc 列（模拟 0.2.10 及更早的库），打开时应自动 ALTER 补列。
void TestLegacySchemaMigration() {
    TempDir dir;
    const auto file = dir.path() / "legacy.db";
    {
        SQLite::Database legacy(file.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
        legacy.exec(R"SQL(
CREATE TABLE requests (
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
INSERT INTO requests(project_id,name,method,url,updated_at)
VALUES(1,'旧请求','GET','https://example.com/legacy',1700000003);
)SQL");
    }

    db::Db handle(file);
    bool hasDoc = false;
    {
        SQLite::Database check(file.string(), SQLite::OPEN_READONLY);
        SQLite::Statement info(check, "PRAGMA table_info(requests)");
        while (info.executeStep())
            if (info.getColumn(1).getString() == "doc") hasDoc = true;
    }
    check(hasDoc, "旧库打开后 requests 补出 doc 列");

    const auto rows = handle.listRequests(1);
    check(rows.size() == 1, "旧库既有行仍可读取");
    if (!rows.empty()) check(rows.front().doc.empty(), "旧行 doc 回落为空串");
}

} // namespace

int main() {
    try {
        TestRoundTrip();
        TestUpdateKeepsDoc();
        TestLegacySchemaMigration();
    } catch (const std::exception& ex) {
        std::println("FAIL: 未捕获异常: {}", ex.what());
        return 1;
    }
    if (failures != 0) {
        std::println("{} 项失败", failures);
        return 1;
    }
    std::println("db 测试通过");
    return 0;
}
