// tcp_engine.cppm — Asio raw TCP / TCPS 异步连接引擎接口。
export module apitab.tcp_engine;

import std;
import apitab.api_engine;

export std::unique_ptr<api::TcpEngine> makeTcpEngine();
