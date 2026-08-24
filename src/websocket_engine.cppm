// websocket_engine.cppm — IXWebSocket 异步连接引擎接口。
export module apitab.websocket_engine;

import std;
import apitab.api_engine;

export std::unique_ptr<api::WebSocketEngine> makeWebSocketEngine();
