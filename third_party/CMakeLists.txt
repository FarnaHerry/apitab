# third_party/CMakeLists.txt — vendor 依赖的解包与构建。
#
# 所有第三方库以 tarball 形式提交在 third_party/tarballs/（来源与版本见
# third_party/README.md），configure 期校验 SHA256 后解包到
# ${CMAKE_BINARY_DIR}/vendor/ 再 add_subdirectory —— 仓库不提交解包后的源码树，
# 构建完全离线、可复现，版本与旧 mcpp.lock 锁定的一致。
#
# 产物 target：
#   HuxerUI::huxerui  HuxerUI 0.2.0 SDK（shared；见文件尾 find_package）
#   asio          Asio 1.38.2（standalone，separate compilation，SSL）+ `import asio` 模块
#   ixwebsocket   IXWebSocket 12.0.1（client-only，无 TLS/无 zlib，与 compat.websocket 一致）
#   CURL::libcurl curl 8.22.0（OpenSSL 后端，静态库，不编 curl.exe）
#   SQLiteCpp     SQLiteCpp 3.3.3（内置 sqlite3 amalgamation）
#   apitab_json   nlohmann::json 3.12.0（single header）
#
# OpenSSL 在顶层 CMakeLists 解析（apitab 的 imported target 需要全局可见），
# 本文件假定 find_package(OpenSSL) 已成功。
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/apitab_vendor.cmake")

if(NOT OpenSSL_FOUND)
    message(FATAL_ERROR "third_party：顶层应已完成 OpenSSL 解析")
endif()

# ---- nlohmann::json 3.12.0（single header + `import nlohmann.json` 模块）------
# cmake/nlohmann.json.cppm 承接原 mcpp nlohmann.json 包生成的模块接口。
add_library(apitab_json STATIC)
target_sources(apitab_json PUBLIC FILE_SET json_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nlohmann.json.cppm")
set_target_properties(apitab_json PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_MODULE_STD ON)
target_include_directories(apitab_json PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/json")

# ---- curl 8.22.0（静态库，OpenSSL 后端，仅 lib）------------------------------
apitab_extract(curl
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/curl-8.22.0.tar.gz"
    "d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1"
    "curl-8.22.0")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
add_subdirectory("${curl_SOURCE_DIR}" "curl-build")


# ---- Asio 1.38.2：standalone + separate compilation + `import asio` 模块 -----
# cmake/asio.cppm 承接原 mcpp chriskohlhoff.asio 包生成的模块接口（SSL 宏改名为
# APITAB_ASIO_SSL）。ASIO_SEPARATE_COMPILATION 必须对模块 TU 和 src/asio.cpp 同时
# 生效，否则 inline 定义会双重编译（见原包描述的注释）。
apitab_extract(asio
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/asio-1.38.2.tar.gz"
    "4893ad026013d1f64bf111c606e6abd857bb953823a4744e449969f1063fd9ce"
    "asio-1.38.2")
add_library(asio STATIC
    "${asio_SOURCE_DIR}/src/asio.cpp"
    "${asio_SOURCE_DIR}/src/asio_ssl.cpp")
target_sources(asio PUBLIC FILE_SET asio_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/asio.cppm")
set_target_properties(asio PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_MODULE_STD ON)
target_compile_definitions(asio PUBLIC ASIO_STANDALONE ASIO_SEPARATE_COMPILATION ASIO_DISABLE_BOOST_CONTEXT_FIBER ASIO_HAS_THREADS ASIO_NO_IOSTREAM APITAB_ASIO_SSL)
target_include_directories(asio PUBLIC "${asio_SOURCE_DIR}/include")
target_link_libraries(asio PUBLIC OpenSSL::SSL OpenSSL::Crypto)
find_package(Threads REQUIRED)
target_link_libraries(asio PUBLIC Threads::Threads)

# ---- IXWebSocket 12.0.1：client-only（与 compat.websocket 的形状一致）--------
# 36 个上游 lib TU 去掉 4 个 server TU 和 3 个 TLS TU = 32 个源文件；
# 无 IXWEBSOCKET_USE_TLS / IXWEBSOCKET_USE_ZLIB，wss 压缩与 TLS 均关闭。
apitab_extract(ixwebsocket
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/ixwebsocket-12.0.1.tar.gz"
    "d23bdc91dbfe2b9ae13c322d539392d7a6b8b506560f41c90e227fa0f86a2405"
    "IXWebSocket-12.0.1")
file(GLOB IXWS_SOURCES "${ixwebsocket_SOURCE_DIR}/ixwebsocket/*.cpp")
list(FILTER IXWS_SOURCES EXCLUDE REGEX
    "(IXSocketServer|IXHttpServer|IXWebSocketServer|IXWebSocketProxyServer|IXSocketOpenSSL|IXSocketMbedTLS|IXSocketAppleSSL)\\.cpp$")
add_library(ixwebsocket STATIC ${IXWS_SOURCES})
target_include_directories(ixwebsocket PUBLIC "${ixwebsocket_SOURCE_DIR}")
target_compile_features(ixwebsocket PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(ixwebsocket PUBLIC ws2_32)
endif()

# ---- SQLiteCpp 3.3.3（内置 sqlite3 amalgamation）-----------------------------
apitab_extract(sqlitecpp
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/SQLiteCpp-3.3.3.tar.gz"
    "33bd4372d83bc43117928ee842be64d05e7807f511b5195f85d30015cad9cac6"
    "SQLiteCpp-3.3.3")
set(SQLITECPP_INTERNAL_SQLITE ON CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${sqlitecpp_SOURCE_DIR}" "SQLiteCpp-build")

# ---- HuxerUI（源码 third_party/huxerui 或预编译 SDK，由顶层选择）--------------
# 替换 EUI-NEO：组件式声明 UI（composable View + UseState + TaskScope）。
# Linux 后端为 GTK4/Cairo，运行时需要系统 libgtk-4-1/libsoup-3.0-0（本机已具备；
# SDK 模式直接链接预编译 libhuxerui.so，源码模式需要 gtk4-devel/libsoup3-devel）。
if (NOT APITAB_HUXERUI_FROM_SOURCE)
    # SDK 自带的资源包（运行时按 HUXERUI_RESOURCES_DIR 或 <exe>.resources/ 查找）
    set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
endif ()
