// log.h — apitab 文件日志（运行诊断 + 控制面审计）。
//
// **规则：日志一律落文件，不进 SQLite。** 日志是高频追加的运维信息：塞进库会与领域
// 读写抢 WAL 的单一写者（每次 append 都是一次事务 + fsync 路径），把库文件撑大，而它
// 对应用的查询毫无价值。领域数据（组织/项目/请求/历史/压测记录）才进库。
// 详见 CLAUDE.md「关键约定」第 10 条。
#pragma once

#include <string>
#include <string_view>

namespace apitab::log {

// 日志目录（缺省 <dataDir>/logs）。可在进程早期显式指定；不调用则首次写入时惰性
// 使用缺省目录。重复调用只生效第一次。
void Initialize(const std::string& directory = {});

// 写一行。线程安全、**不抛异常**：写文件失败静默降级到 stderr（诊断不能反过来拖垮
// 调用方）。行格式：`2026-09-26 21:37:04.123 [INFO] message`。
void Write(std::string_view level, std::string_view message);
void Info(std::string_view message);
void Warn(std::string_view message);
void Error(std::string_view message);

// 当前日志文件路径（诊断/测试用；未初始化且从未写入时为空）。
std::string CurrentLogPath();

} // namespace apitab::log
