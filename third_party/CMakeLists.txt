# third_party/CMakeLists.txt — vendor 依赖的解包与构建。
#
# 所有第三方库以 tarball 形式提交在 third_party/tarballs/（来源与版本见
# third_party/README.md），configure 期校验 SHA256 后解包到
# ${CMAKE_BINARY_DIR}/vendor/ 再 add_subdirectory —— 仓库不提交解包后的源码树，
# 构建完全离线、可复现，版本与旧 mcpp.lock 锁定的一致。
#
# 产物 target：
#   eui::neo      EUI-NEO 0.5.7（bundled 3rd/，glfw + opengl，无 tray/markdown）
#   asio          Asio 1.38.1（standalone，separate compilation，SSL）+ `import asio` 模块
#   ixwebsocket   IXWebSocket 12.0.1（client-only，无 TLS/无 zlib，与 compat.websocket 一致）
#   CURL::libcurl curl 8.21.0（OpenSSL 后端，静态库，不编 curl.exe）
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

# ---- curl 8.21.0（静态库，OpenSSL 后端，仅 lib）------------------------------
# 必须在 eui 之前：eui 的 bundled 依赖脚本对非 Windows find_package(CURL REQUIRED)，
# 并以 TARGET CURL::libcurl 判定是否给 eui_neo 定义 EUI_HAS_CURL=1（旧 mcpp 构建带
# 网络支持，保持一致）。
apitab_extract(curl
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/curl-8.21.0.tar.gz"
    "ec753aa6f408a3ca9f0d6d5f7a77417aecd1544db13c03ae5d443612bf367364"
    "curl-curl-8_21_0")
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
if(NOT WIN32)
    # 给 eui 子树里的 find_package(CURL) 直接播种结果，绕开系统搜索
    set(CURL_INCLUDE_DIR "${curl_SOURCE_DIR}/include" CACHE PATH "vendored curl" FORCE)
    set(CURL_LIBRARY "${CMAKE_BINARY_DIR}/curl-build/libcurl.a" CACHE FILEPATH "vendored curl" FORCE)
endif()

# ---- Asio 1.38.1：standalone + separate compilation + `import asio` 模块 -----
# cmake/asio.cppm 承接原 mcpp chriskohlhoff.asio 包生成的模块接口（SSL 宏改名为
# APITAB_ASIO_SSL）。ASIO_SEPARATE_COMPILATION 必须对模块 TU 和 src/asio.cpp 同时
# 生效，否则 inline 定义会双重编译（见原包描述的注释）。
apitab_extract(asio
    "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/asio-1.38.1.tar.gz"
    "2827b229972be80cdb14e5497962fa393d1adf036b5869e2b9c99f644daadacc"
    "asio-asio-1-38-1")
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

# ---- Linux 本地兜底：系统缺 X11/GL 开发头时借用 mcpp subos 的预编译视图 ------
# CI（ubuntu/macos）有系统包，不会进这个分支；只服务本机无 sudo 装包的场景。
# glfw 的 X11 后端硬性要求 Xrandr/Xinerama/Xcursor/Xi 头，缺了直接 FATAL。
set(APITAB_X11_FALLBACK_PREFIX "")
if(UNIX AND NOT APPLE)
    find_path(APITAB_SYSTEM_XRANDR_HEADER X11/extensions/Xrandr.h)
    if(NOT APITAB_SYSTEM_XRANDR_HEADER)
        set(_mcpp_subos "$ENV{HOME}/.mcpp/registry/subos/default")
        if(EXISTS "${_mcpp_subos}/usr/include/X11/extensions/Xrandr.h")
            # 只挑选 X11/GL 相关的头和库，绝不整个 include/lib 链接进来：
            # subos 视图里还有私有 glibc / 旧 libstdc++，全量借用会劫持系统链接。
            set(_stage "${APITAB_VENDOR_DIR}/x11-stage")
            file(MAKE_DIRECTORY "${_stage}/include" "${_stage}/lib")
            if(NOT EXISTS "${_stage}/include/X11")
                file(CREATE_LINK "${_mcpp_subos}/usr/include/X11" "${_stage}/include/X11" SYMBOLIC)
            endif()
            foreach(_xlib X11 X11-xcb xcb Xau Xdmcp Xcursor Xext Xfixes Xinerama Xi
                          Xrandr Xrender Xxf86vm GL GLX OpenGL EGL GLESv2 GLdispatch
                          glvnd wayland-client wayland-cursor wayland-egl xkbcommon
                          xkbcommon-x11)
                file(GLOB _xlib_links "${_mcpp_subos}/lib/lib${_xlib}.so*")
                foreach(_xl ${_xlib_links})
                    get_filename_component(_xname "${_xl}" NAME)
                    if(NOT EXISTS "${_stage}/lib/${_xname}")
                        file(CREATE_LINK "${_xl}" "${_stage}/lib/${_xname}" SYMBOLIC)
                    endif()
                endforeach()
            endforeach()
            unset(_xlib)
            unset(_xlib_links)
            unset(_xname)
            set(APITAB_X11_FALLBACK_PREFIX "${_stage}")
            message(STATUS "apitab: 系统 X11 开发头缺失，借用 mcpp subos 的 X11/GL 子集")
        else()
            message(FATAL_ERROR
                "缺 X11 开发头（libxrandr-devel 等）。请 `sudo dnf install libXrandr-devel "
                "libXcursor-devel libXinerama-devel libXi-devel`，或保留 mcpp 的 "
                "~/.mcpp/registry/subos/default 供本构建借用。")
        endif()
    endif()
endif()

# ---- HuxerUI 0.1.0（官方预编译 SDK，shared 库）--------------------------------
# 替换 EUI-NEO：组件式声明 UI（composable View + UseState + TaskScope）。
# Linux 后端为 GTK4/Cairo，运行时需要系统 libgtk-4-1/libsoup-3.0-0（本机已具备，
# 无需 dev 头文件 —— 直接链接 SDK 的 libhuxerui.so）。
if(APPLE)
    apitab_extract(huxerui
        "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/huxerui-sdk-0.1.0-macos-arm64.tar.gz"
        "f1f3d115f0647d43e8149c82be1e628605fbef4414bf75bd1d7b9de7b1df0852"
        "huxerui-sdk-0.1.0-macos-arm64")
elseif(WIN32)
    apitab_extract(huxerui
        "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/huxerui-sdk-0.1.0-windows-x86_64.zip"
        "8d8863878a56b38ad1e669ba666297cc0526e896f3d5e32aabf8aeaaf0b68539"
        "huxerui-sdk-0.1.0-windows-x86_64")
else()
    apitab_extract(huxerui
        "${CMAKE_CURRENT_SOURCE_DIR}/tarballs/huxerui-sdk-0.1.0-linux-x86_64.tar.gz"
        "147833204b7a08bdae28a2d66019679ac0814af2d3d7238c7a6ea6e39fb1c5a3"
        "huxerui-sdk-0.1.0-linux-x86_64")
endif()
set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root")

add_library(huxerui SHARED IMPORTED GLOBAL)
set_target_properties(huxerui PROPERTIES IMPORTED_GLOBAL TRUE
    INTERFACE_INCLUDE_DIRECTORIES "${HUXERUI_SDK_DIR}/include")
if(WIN32)
    set_target_properties(huxerui PROPERTIES
        IMPORTED_IMPLIB "${HUXERUI_SDK_DIR}/lib/huxerui.lib"
        IMPORTED_LOCATION "${HUXERUI_SDK_DIR}/bin/huxerui.dll")
else()
    set_target_properties(huxerui PROPERTIES
        IMPORTED_LOCATION "${HUXERUI_SDK_DIR}/lib/libhuxerui.so")
endif()
# SDK 自带的资源包（运行时按 HUXERUI_RESOURCES_DIR 或 <exe>.resources/ 查找）
set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")

