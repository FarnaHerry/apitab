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

namespace {
std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// URL 分段拼接：去掉段首 '/' 与已有末尾 '/'，非空时用一个 '/' 连接。
// composeUrl / urlPrefix 共用，保证"前缀段显示"与"实际拼接"永远同规则。
void appendUrlSegment(std::string& url, std::string_view segment) {
    while (!segment.empty() && segment.front() == '/') segment.remove_prefix(1);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (segment.empty()) return;
    if (!url.empty()) url += '/';
    url += segment;
}

// ---- Set-Cookie 解析（响应 Cookie 归集到项目 Cookie 用）----
// 项目 Cookie 的模型是项目级 name/value（没有 domain/path 维度），所以这里只取名字
// 与值；属性段仅用来判定"这次响应是不是在删掉这个 Cookie"。
struct SetCookieField {
    std::string name;
    std::string value;
    bool expired = false; // Max-Age<=0，或 Expires 已过期 → 应从项目 Cookie 删除
};

// RFC 1123 日期（"Sun, 06 Nov 1994 08:49:37 GMT"，末尾时区可省）以及 asctime 形式
// （"Sun Nov  6 08:49:37 1994"）→ Unix 秒；解析失败返回 nullopt（按会话 Cookie
// 处理，不删）。只服务 Expires 属性：用 chrono 日历类型换算，不依赖 timegm
//（MSVC 没有）。
std::optional<std::int64_t> parseHttpDate(std::string_view raw) {
    std::string text{raw};
    if (const std::size_t comma = text.find(','); comma != std::string::npos)
        text.erase(0, comma + 1); // 去掉可选的 "Wdy," 前缀
    std::vector<std::string> token;
    for (std::size_t i = 0; i < text.size();) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        const std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i > start) token.push_back(text.substr(start, i - start));
    }
    static constexpr std::array<std::string_view, 12> kMonths{
        "jan", "feb", "mar", "apr", "may", "jun",
        "jul", "aug", "sep", "oct", "nov", "dec"};
    const auto monthOf = [](std::string_view name) -> std::optional<unsigned> {
        const std::string lower = lowerAscii(std::string{name});
        const auto found = std::ranges::find(kMonths, std::string_view{lower});
        if (found == kMonths.end()) return std::nullopt;
        return static_cast<unsigned>(found - kMonths.begin()) + 1U;
    };
    const auto toInt = [](std::string_view s) -> std::optional<int> {
        int value = 0;
        const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
        return value;
    };
    // 先找到月份 token（最多看前三个）：三种在野外出现的字段顺序——
    //   asctime 带星期（"Thu Jan  1 00:00:00 1970"，日期补空格所以天数是两个空白）
    //   asctime 不带星期（"Jan  1 00:00:00 1970"）
    //   RFC 1123（"01 Jan 1970 00:00:00 GMT"，末尾时区可省）
    // 用「月份在第几个 token」+「首个 token 是不是数字」区分，不要按 token 数量判断。
    std::optional<unsigned> month;
    std::size_t month_at = 0;
    for (std::size_t i = 0; i < token.size() && i < 3; ++i) {
        if (const std::optional<unsigned> found = monthOf(token[i])) {
            month = found;
            month_at = i;
            break;
        }
    }
    if (!month) return std::nullopt; // 也顺带挡掉不支持的 RFC 850 "01-Jan-70"
    std::optional<int> day;
    std::optional<int> year;
    std::string_view clock;
    if (month_at == 0) { // asctime（无星期）
        if (token.size() < 4) return std::nullopt;
        day = toInt(token[1]);
        clock = token[2];
        year = toInt(token[3]);
    } else if (month_at == 1 && toInt(token[0]).has_value()) { // RFC 1123
        if (token.size() < 4) return std::nullopt;
        day = toInt(token[0]);
        year = toInt(token[2]);
        clock = token[3];
    } else if (month_at == 1) { // asctime 带星期
        if (token.size() < 5) return std::nullopt;
        day = toInt(token[2]);
        clock = token[3];
        year = toInt(token[4]);
    } else {
        return std::nullopt;
    }
    if (!day || !year) return std::nullopt;
    const std::size_t first = clock.find(':');
    const std::size_t second =
        first == std::string_view::npos ? std::string_view::npos : clock.find(':', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos) return std::nullopt;
    const std::optional<int> hour = toInt(clock.substr(0, first));
    const std::optional<int> minute = toInt(clock.substr(first + 1, second - first - 1));
    const std::optional<int> second_of_minute = toInt(clock.substr(second + 1));
    if (!hour || !minute || !second_of_minute) return std::nullopt;
    const std::chrono::year_month_day ymd{
        std::chrono::year{*year}, std::chrono::month{*month},
        std::chrono::day{static_cast<unsigned>(*day)}};
    if (!ymd.ok()) return std::nullopt;
    const auto point = std::chrono::sys_days{ymd} + std::chrono::hours{*hour} +
                       std::chrono::minutes{*minute} + std::chrono::seconds{*second_of_minute};
    return std::chrono::duration_cast<std::chrono::seconds>(point.time_since_epoch()).count();
}

// 抽出响应里的全部 Set-Cookie 字段（键大小写不敏感）。一行只当一个 Cookie：
// Expires 里带逗号，按逗号拆多 Cookie 会把日期拆坏（RFC 6265 也禁止折叠）。
std::vector<SetCookieField> parseSetCookies(const std::vector<api::KeyValue>& headers,
                                           std::int64_t now) {
    std::vector<SetCookieField> out;
    for (const api::KeyValue& header : headers) {
        if (lowerAscii(header.key) != "set-cookie") continue;
        const std::string_view raw{header.value};
        const std::size_t semi = raw.find(';');
        const std::string_view pair = raw.substr(0, semi);
        const std::size_t eq = pair.find('=');
        if (eq == std::string_view::npos) continue; // 没有 '=' 不是合法 Cookie
        SetCookieField field;
        field.name = trim(std::string{pair.substr(0, eq)});
        if (field.name.empty()) continue;
        field.value = trim(std::string{pair.substr(eq + 1)});
        if (field.value.size() >= 2 && field.value.front() == '"' && field.value.back() == '"')
            field.value = field.value.substr(1, field.value.size() - 2); // 成对引号
        // 属性段：Max-Age 优先于 Expires（RFC 6265 §4.1.2.2）
        std::optional<std::int64_t> max_age;
        std::optional<std::int64_t> expires_at;
        std::string_view attr_text =
            semi == std::string_view::npos ? std::string_view{} : raw.substr(semi + 1);
        while (!attr_text.empty()) {
            const std::size_t next = attr_text.find(';');
            const std::string_view attr = attr_text.substr(0, next);
            attr_text = next == std::string_view::npos ? std::string_view{}
                                                       : attr_text.substr(next + 1);
            const std::size_t attr_eq = attr.find('=');
            if (attr_eq == std::string_view::npos) continue;
            const std::string key = lowerAscii(trim(std::string{attr.substr(0, attr_eq)}));
            const std::string value = trim(std::string{attr.substr(attr_eq + 1)});
            if (key == "max-age") {
                std::int64_t seconds = 0;
                const auto [end, ec] =
                    std::from_chars(value.data(), value.data() + value.size(), seconds);
                if (ec == std::errc{} && end == value.data() + value.size()) max_age = seconds;
            } else if (key == "expires") {
                if (const std::optional<std::int64_t> parsed = parseHttpDate(value))
                    expires_at = parsed;
            }
        }
        if (max_age.has_value()) field.expired = *max_age <= 0;
        else if (expires_at.has_value()) field.expired = *expires_at <= now;
        out.push_back(std::move(field));
    }
    return out;
}

} // namespace

export class RequestStore {
public:
    // 静态初始化保持**廉价**：只建一个空壳。CLI 客户端进程（`apitab --cli …`）只做
    // 控制面转发，不该因为模块级全局构造就打开数据库、起 curl 工作线程（旧实现在
    // 客户端用假 HOME 跑会建出空库，正是"两进程抢 WAL 写者"的来源）。
    RequestStore() = default;

    // 打开库与引擎（幂等）。GUI 进程在进事件循环前调用一次；控制面命令入口再兜一次
    // （服务端命令跑到这里时库早已打开，这只是一次 bool 检查）。
    void Open() {
        if (opened_) return;
        opened_ = true;
        engine_ = makeCurlEngine();
        const Status initialized = captureResult([&] {
            db_ = std::make_unique<db::Db>(cfg::databaseFile());
            // 迁移兜底：空库建默认组织/项目，游离请求归入默认项目。
            db_->ensureDefaultProject();
            reloadOrgs();
        });
        healthy_ = initialized.has_value();
        if (!initialized) {
            startupError_ = initialized.error();
        }
    }

    [[nodiscard]] bool opened() const { return opened_; }

    RequestStore(const RequestStore&) = delete;
    RequestStore& operator=(const RequestStore&) = delete;

    // ---- 组织 ----

    const std::vector<db::Org>& orgs() const { return orgs_; }
    std::int64_t currentOrgId() const { return currentOrgId_; }

    Status selectOrg(std::int64_t id) {
        return guarded([&] {
            if (currentProjectId_ != 0) {
                selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            }
            currentOrgId_ = id;
            reloadProjects();
        });
    }

    Status createOrg(const std::string& name) {
        return guarded([&] {
            currentOrgId_ = db_->createOrg(name);
            reloadOrgs();
        });
    }

    Status renameOrg(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameOrg(id, name);
            reloadOrgs();
        });
    }

    // 删除组织（级联删项目与请求）。
    Status deleteOrg(std::int64_t id) {
        return guarded([&] {
            db_->deleteOrg(id);
            db_->ensureDefaultProject();  // 删光后保底
            reloadOrgs();
        });
    }

    // ---- 项目 ----

    const std::vector<db::Project>& projects() const { return projects_; }
    Result<std::vector<db::Project>> allProjects() {
        return databaseResult([&] {
            std::vector<db::Project> result;
            for (const auto& org : orgs_) {
                std::vector<db::Project> projects = db_->listProjects(org.id);
                result.insert(result.end(), std::make_move_iterator(projects.begin()),
                              std::make_move_iterator(projects.end()));
            }
            return result;
        });
    }
    std::int64_t currentProjectId() const { return currentProjectId_; }

    Status selectProjectInOrg(std::int64_t orgId, std::int64_t projectId) {
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

    Status selectProject(std::int64_t id) {
        return guarded([&] {
            if (currentProjectId_ != 0) {
                selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            }
            currentProjectId_ = id;
            reloadGroups();
        });
    }

    Status createProject(const std::string& name) {
        return guarded([&] {
            currentProjectId_ = db_->createProject(currentOrgId_, name);
            (void)db_->createEnvironment(currentProjectId_, "localhost", "http://localhost");
            reloadProjects();
        });
    }

    Status renameProject(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameProject(id, name);
            reloadProjects();
        });
    }

    // 项目设置页：全量更新名称/说明/公共请求头。
    Status updateProjectMeta(std::int64_t id, const std::string& name,
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
    // 删除项目（级联删其请求）。
    Status deleteProject(std::int64_t id) {
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
    Status restoreEnvironment(std::int64_t projectId, std::int64_t envId) {
        if (Status selected = selectProject(projectId); !selected) return selected;
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
    Status selectEnv(std::int64_t id) {
        return guarded([&] {
            if (id != 0 && !findEnvironment(id)) {
                throw std::runtime_error("环境不属于当前项目");
            }
            currentEnvId_ = id;
            selectedEnvByProject_[currentProjectId_] = id;
        });
    }

    Status createEnvironment(const std::string& name, const std::string& baseUrl) {
        return guarded([&] {
            currentEnvId_ = db_->createEnvironment(currentProjectId_, name, baseUrl);
            selectedEnvByProject_[currentProjectId_] = currentEnvId_;
            reloadEnvironments();
        });
    }

    Status renameEnvironment(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameEnvironment(id, name);
            reloadEnvironments();
        });
    }

    Status setEnvironmentBaseUrl(std::int64_t id, const std::string& baseUrl) {
        return guarded([&] {
            db_->setEnvironmentBaseUrl(id, baseUrl);
            reloadEnvironments();
        });
    }

    Status setEnvironmentVariables(std::int64_t id,
                                   const std::vector<api::KeyValue>& vars) {
        return guarded([&] {
            db_->setEnvironmentVariables(id, vars);
            reloadEnvironments();
        });
    }

    Status updateEnvironment(std::int64_t id, const std::string& name,
                             const std::string& baseUrl,
                             const std::vector<api::KeyValue>& variables) {
        return guarded([&] {
            db_->renameEnvironment(id, name);
            db_->setEnvironmentBaseUrl(id, baseUrl);
            db_->setEnvironmentVariables(id, variables);
            reloadEnvironments();
        });
    }

    Status deleteEnvironment(std::int64_t id) {
        return guarded([&] {
            db_->deleteEnvironment(id);
            reloadEnvironments();
        });
    }

    // 组装最终 URL：baseUrl + 目录 Path 链 + path
    std::string composeUrl(const std::string& path, std::int64_t groupId,
                           std::int64_t envId) const {
        const std::string cleanPath = trim(path);
        // 显式 URI scheme 是完整目标，不再叠加环境或 Path 分组。
        if (hasUriScheme(cleanPath)) return cleanPath;

        std::string url;
        if (const db::Environment* e = findEnvironment(envId)) {
            url = trim(e->baseUrl);
        }
        appendUrlSegment(url, groupPathChain(groupId));
        appendUrlSegment(url, cleanPath);
        return url;
    }

    // 最终 URL 的前缀部分（当前环境 baseUrl + 目录 Path 链），不含请求自己的路径。
    // URL 行的只读前缀段用它显示"拼接结果的前半截"（输入框里是后半截），这样目录
    // 增加的路由是看得见的、不用猜。带 URI scheme 的输入不参与拼接，调用方按
    // hasUriScheme 弱化/隐藏（与 composeUrl、finalizeSpec 同一条规则）。
    std::string urlPrefix(std::int64_t groupId, std::int64_t envId) const {
        std::string prefix;
        if (const db::Environment* e = findEnvironment(envId)) prefix = trim(e->baseUrl);
        appendUrlSegment(prefix, groupPathChain(groupId));
        return prefix;
    }

    // 从根到该目录的 Path 链（只取 Path 模式目录的 path；Name 模式目录不贡献路由），
    // 用 '/' 连接。嵌套目录逐级累加：api(P) > v1(P) → "api/v1"。
    std::string groupPathChain(std::int64_t groupId) const {
        std::vector<std::string> segments;
        std::int64_t cursor = groupId;
        // 环保护：脏数据（parent 指回自己/互指）不至于死循环。
        for (std::size_t guard = 0; cursor != 0 && guard < 64; ++guard) {
            const db::Group* g = findGroup(cursor);
            if (!g) break;
            if (g->mode == db::GroupMode::Path) {
                const std::string value = groupPathValue(*g);
                if (!value.empty()) segments.push_back(value);
            }
            cursor = g->parentId;
        }
        std::string chain;
        for (auto it = segments.rbegin(); it != segments.rend(); ++it) { // 根 → 叶
            if (!chain.empty()) chain += '/';
            chain += *it;
        }
        return chain;
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

    // Path 分组的实际 URL 前缀：path 列空时回落 name（兼容旧数据）。
    static std::string groupPathValue(const db::Group& g) {
        return g.path.empty() ? g.name : g.path;
    }

    Status createGroup(const std::string& name, db::GroupMode mode,
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

    Status renameGroup(std::int64_t id, const std::string& name) {
        return guarded([&] {
            db_->renameGroup(id, name);
            reloadGroups();
        });
    }

    // 接口目录编辑必须原子同步显示名称、URL 路径和目录模式；旧 renameGroup
    // 只改 name，会让 Path 目录继续使用过期 path。
    Status updateGroup(std::int64_t id, const std::string& name,
                       const std::string& path) {
        return guarded([&] {
            if (!findGroup(id)) throw std::runtime_error("接口目录不存在");
            db_->updateGroup(id, name,
                             path.empty() ? db::GroupMode::Name : db::GroupMode::Path,
                             path);
            reloadGroups();
        });
    }

    Status setGroupMode(std::int64_t id, db::GroupMode mode) {
        return guarded([&] {
            db_->setGroupMode(id, mode);
            reloadGroups();
        });
    }

    Status deleteGroup(std::int64_t id) {
        return guarded([&] {
            db_->deleteGroup(id);
            reloadGroups();
        });
    }

    // 把请求移到某分组（0 = 未分组）。
    Status moveToGroup(std::int64_t requestId, std::int64_t groupId) {
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
    Status moveGroup(std::int64_t id, std::int64_t parentId) {
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

    // 保存（新建 id==0 / 更新）。失败返回结构化错误。
    Status save(db::SavedRequest& r) {
        if (currentProjectId_ == 0)
            return std::unexpected(AppError{"未打开项目，无法保存"});
        r.projectId = currentProjectId_;
        r.updatedAt = nowUnix();
        return databaseResult([&] {
            const std::int64_t id = db_->saveRequest(r);
            r.id = id;
            reloadRequests();
        });
    }

    Status remove(std::int64_t id) {
        return databaseResult([&] {
            db_->deleteRequest(id);
            reloadRequests();
        });
    }

    // 重命名已保存请求：改字段走 saveRequest 更新（同 moveToGroup 的思路）。
    Status renameRequest(std::int64_t id, const std::string& name) {
        return guarded([&] {
            if (const db::SavedRequest* r = find(id)) {
                db::SavedRequest copy = *r;
                copy.name = name;
                db_->saveRequest(copy);
                reloadRequests();
            }
        });
    }

    Result<std::vector<db::GlobalCookie>> globalCookies() {
        return databaseResult([&] { return db_->listGlobalCookies(currentProjectId_); });
    }
    Status saveGlobalCookie(db::GlobalCookie& cookie) {
        if (cookie.projectId != 0 && cookie.projectId != currentProjectId_)
            return std::unexpected(AppError{"项目不匹配"});
        cookie.projectId = currentProjectId_;
        return databaseResult([&] {
            cookie.id = db_->saveGlobalCookie(cookie);
        });
    }
    Status deleteGlobalCookie(std::int64_t id) {
        return databaseResult([&] { db_->deleteGlobalCookie(id, currentProjectId_); });
    }

    // ---- 发送（传输由引擎抽象 api::ApiEngine 执行，当前为 curl 实现）----

    // 派发一次请求（异步）：引擎 send 纯入队立即返回，结果由 UI 侧轮询
    // takeResponse 取走；busy 时再调视为替换（取消旧请求、发起新请求）。
    void sendViaEngine(const api::RequestSpec& spec) { engine_->send(spec); }
    // 协作式取消在途请求（不保证立即生效）；丢弃排队请求，取消后结果不投递。
    void cancelSend() { engine_->cancel(); }
    // UI 线程轮询：引擎有新完成的结果则取出并返回 true（一个结果只取一次）。
    bool takeResponse(api::ResponseView& out) { return engine_->takeResponse(out); }
    // UI 线程轮询：在途传输有增量（响应头到达/正文新增）则取出当前累积快照。
    // SSE 等流式响应经此呈现；快照按代际隔离，传输结束后进度槽即清空。
    bool takeProgress(api::ResponseView& out) { return engine_->takeProgress(out); }

    // 组装最终请求规格：环境变量替换 + 基础 URL 拼接 + url 拼启用的 query 参数，
    // 合并全局 Cookie。
    // 先替换当前环境的 {{变量}}，再拼 URL（保证变量值里的特殊字符被正确百分号编码）。
    Result<api::RequestSpec> finalizeSpec(const api::RequestSpec& spec) {
        return databaseResult([&] {
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
        // 基础 URL 拼接：输入（变量替换后）无 URI scheme 时按 baseUrl + 目录 Path
        // 链 + 路径组装（composeUrl 内部分段去重斜杠）；带 scheme 的完整 URL 原样
        // 保留、不再叠加任何前缀（"目录即路由"也一并失效）。目录取自需求规格的
        // groupId（GUI 从草稿、CLI 从集合项带入）。
        finalSpec.url = composeUrl(finalSpec.url, finalSpec.groupId, currentEnvId_);
        std::vector<std::pair<std::string, std::string>> enabled;
        for (const auto& p : finalSpec.params) {
            if (p.enabled && !p.key.empty()) enabled.emplace_back(p.key, p.value);
        }
        const std::vector<db::GlobalCookie> cookies = db_->listGlobalCookies(currentProjectId_);
        for (const auto& cookie : cookies) {
            if (cookie.enabled) finalSpec.cookies.push_back({cookie.name, cookie.value, true, {}, {}});
        }
        // JSON 体剥离注释（引擎不处理，见 curl_engine.cpp；原由已删除的
        // http_build 承担，随发送切回引擎移入本函数）。
        if (finalSpec.bodyKind == api::BodyKind::Json && finalSpec.allowJsonComments) {
            finalSpec.body = stripJsonComments(finalSpec.body);
        }
        finalSpec.url = appendQuery(trim(finalSpec.url), enabled);
        // ---- 项目公共头合并 + 全局超时/代理注入 ----
        // 同名键（大小写不敏感）请求显式头优先；环境变量替换已在前面完成，
        // 项目公共头不参与 {{var}} 替换（配置侧直接写死值）。
        std::vector<api::KeyValue> inherited;
        for (const db::Project& p : projects_) {
            if (p.id == currentProjectId_)
                for (const api::KeyValue& h : p.headers) inherited.push_back(h);
        }
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
        });
    }

    // 历史落库：响应回到 UI 线程后由页面调用；写入失败不打断主流程。
    // requestId 关联集合请求（未保存的草稿 = 0）。
    Status recordHistory(std::int64_t requestId, const std::string& method,
                         const std::string& url, const api::ResponseView& result) {
        return databaseResult([&] {
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
        });
    }

    // 响应 Cookie 归集：把响应里的 Set-Cookie 并入当前项目的项目 Cookie。
    // 与 recordHistory 成对调用——响应回到 UI 线程后的收尾，GUI（request_editor.cpp）
    // 与 CLI（cli.cpp）两条发送路径都要调（漏调的那条会静默丢掉响应 Cookie）。
    // 语义：
    //   - 同名（Cookie 名大小写敏感，RFC 6265）覆盖值；用户显式停用的条目保留停用，
    //     不擅自打开（值取服务器最新的，启用与否是用户的选择）；
    //   - 首次见到的 Cookie 写成启用；
    //   - 删除语义（Max-Age<=0 或 Expires 已过期）删掉同名条目——登出流程靠它；
    //   - 项目 Cookie 没有 domain/path 维度，任何响应返回的 Cookie 都进本项目列表，
    //     并随本项目之后每次发送一起带上（既有模型，见 finalizeSpec）。
    // 返回发生增/改/删的条数；写入失败通过错误结果交给调用方决定如何处理。
    Result<std::size_t> collectResponseCookies(const api::ResponseView& result) {
        return databaseResult([&] {
        const std::vector<SetCookieField> incoming = parseSetCookies(result.headers, nowUnix());
        if (incoming.empty()) return std::size_t{0};
        std::size_t changed = 0;
            std::vector<db::GlobalCookie> stored = db_->listGlobalCookies(currentProjectId_);
            for (const SetCookieField& field : incoming) {
                const auto found = std::ranges::find_if(
                    stored, [&](const db::GlobalCookie& c) { return c.name == field.name; });
                if (field.expired) {
                    if (found == stored.end()) continue;
                    db_->deleteGlobalCookie(found->id, currentProjectId_);
                    stored.erase(found);
                    ++changed;
                    continue;
                }
                if (found == stored.end()) {
                    db::GlobalCookie fresh{.projectId = currentProjectId_,
                                           .name = field.name,
                                           .value = field.value,
                                           .enabled = true};
                    fresh.id = db_->saveGlobalCookie(fresh);
                    stored.push_back(std::move(fresh));
                    ++changed;
                } else if (found->value != field.value) {
                    found->value = field.value;
                    db_->saveGlobalCookie(*found);
                    ++changed;
                }
            }
        return changed;
        });
    }

    Result<std::vector<db::HistoryEntry>> history(int limit = 50) {
        return databaseResult([&] { return db_->listHistory(limit); });
    }

    Result<std::int64_t> historyCount() {
        return databaseResult([&] { return db_->historyCount(); });
    }

    Result<std::vector<db::HistoryEntry>> historyPage(int pageSize, std::int64_t pageIndex) {
        return databaseResult([&] {
            const int safePageSize = std::clamp(pageSize, 1, 100);
            const std::int64_t offset = std::max<std::int64_t>(0, pageIndex) * safePageSize;
            return db_->listHistoryPage(safePageSize, offset);
        });
    }

    Status clearHistory() {
        return databaseResult([&] { db_->clearHistory(); });
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

    template <typename F>
    auto databaseResult(F&& fn) -> Result<std::invoke_result_t<F>> {
        if (!db_) {
            healthy_ = false;
            return std::unexpected(startupError_.value_or(AppError{"数据库不可用"}));
        }
        auto result = captureResult(std::forward<F>(fn));
        healthy_ = result.has_value();
        return result;
    }

    // 对外命令边界统一把 SQLiteCpp / 校验异常转换成错误值。
    template <typename F>
    Status guarded(F&& fn) {
        return databaseResult([&] { std::invoke(std::forward<F>(fn)); });
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
    std::optional<AppError> startupError_;
    bool opened_ = false;
    bool healthy_ = true;
};

// 领域单例（importers 间共享同一实体）。**打开是显式的**：GUI 进程在进事件循环前
// 调 g_requests.Open()，控制面命令入口兜底；CLI 客户端进程不调用 → 不碰数据库。
export RequestStore g_requests;
