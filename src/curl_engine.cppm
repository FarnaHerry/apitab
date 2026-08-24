// curl_engine.cppm — apitab.curl_engine：libcurl 实现的单次请求引擎。
// 工厂接口模块：curl 头只进实现单元（curl_engine.cpp 的全局模块片段），
// 不污染本接口的 importers。
export module apitab.curl_engine;

import std;
import apitab.api_engine;

// 创建 curl 引擎实例（领域 store 持有，UI 线程创建/销毁）。
export std::unique_ptr<api::ApiEngine> makeCurlEngine();
