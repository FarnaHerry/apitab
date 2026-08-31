// k6_engine.cppm — apitab.k6_engine：k6 外部进程实现的压测引擎。
// 工厂接口模块：进程/系统头只进实现单元（k6_engine.cpp 的全局模块片段）。
export module apitab.k6_engine;

import std;
import apitab.api_engine;

// 创建 k6 引擎实例。binary 为已解析的 k6 可执行文件路径（空 = 不可用，
// available() 返回 false，start 直接报错）。
export std::unique_ptr<api::LoadEngine> makeK6Engine(std::string binaryPath);

namespace api {

// 由请求参数生成 k6 脚本模板 —— 与 start() 的自动生成路径是同一份逻辑。
// 压测页脚本编辑器用它做初始内容；用户改过的脚本经 LoadOptions.script 回传。
export std::string BuildScript(const RequestSpec& spec, const LoadOptions& opts);

} // namespace api
