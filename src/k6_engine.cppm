// k6_engine.cppm — apitab.k6_engine：k6 外部进程实现的压测引擎。
// 工厂接口模块：进程/系统头只进实现单元（k6_engine.cpp 的全局模块片段）。
export module apitab.k6_engine;

import std;
import apitab.api_engine;

// 创建 k6 引擎实例。binary 为已解析的 k6 可执行文件路径（空 = 不可用，
// available() 返回 false，start 直接报错）。
export std::unique_ptr<api::LoadEngine> makeK6Engine(std::string binaryPath);
