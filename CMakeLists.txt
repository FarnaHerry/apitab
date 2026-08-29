# CMakeLists.txt — apitab 顶层构建（自 mcpp 迁移，功能等价：见 docs/ 下迁移说明）。
#
# 形状：UI 层是普通 C++ 源（huxerui.h + [[huxerui::composable]]，经 huxerui_add_app
# 的 codegen 处理）；领域层（store/引擎/db/config/preferences）是 apitab.* C++ 模块
# （FILE_SET 追加到同一目标）。依赖 vendor 在 third_party/，SDK 经 find_package 消费。
#
# 常用命令：
#   cmake -B build -G Ninja          # 配置（可用 -DCMAKE_BUILD_TYPE=Release）
#   cmake --build build              # 编译
#   ./run.sh                         # 运行 GUI（工作目录 = 仓库根，assets/ 字体可被解析）
#   ctest --test-dir build           # 冒烟测试
cmake_minimum_required(VERSION 3.30)

# `import std` 的 experimental 门控必须在 project() 前设置（UUID 随 CMake 版本变，
# 见 cmake/CxxImportStdGate.cmake 顶部注释）。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CxxImportStdGate)
# 只给用 import std 的目标开 CXX_MODULE_STD（apitab / test_smoke），
# 不影响 eui / curl 等 C++17 子项目。
set(CMAKE_CXX_MODULE_STD OFF)

project(apitab VERSION 0.1.0 DESCRIPTION "API testing and load testing tool" LANGUAGES C CXX)
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)            # gnu++23：模块实现与 GCC 标准库组合最顺
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 默认 Release（单配置生成器）；调试请显式 -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

# ---- OpenSSL 解析（必须在顶层：imported target 要对 apitab 全局可见）---------
# 优先系统 OpenSSL（CI: ubuntu libssl-dev / macos brew openssl@3 / windows vcpkg）；
# 系统没有时，linux x86_64 使用 vendor 的静态 3.5.1（由原 compat.openssl 管线构建）。
find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
        include(cmake/apitab_vendor.cmake)
        apitab_extract(openssl
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/openssl-3.5.1-linux-x86_64.tar.gz"
            "d749ee7ec6bda301dba4a67dc7196d4a04b310f664dc360d6ab2e49298b731cf"
            "openssl-3.5.1-linux-x86_64")
        set(OPENSSL_ROOT_DIR "${openssl_SOURCE_DIR}" CACHE PATH "vendored OpenSSL" FORCE)
        set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
        find_package(OpenSSL REQUIRED)
        message(STATUS "apitab: using vendored OpenSSL ${OPENSSL_VERSION} (static)")
    else()
        message(FATAL_ERROR
            "未找到 OpenSSL 开发头文件/库。请安装系统 OpenSSL（libssl-dev / "
            "openssl@3 / vcpkg openssl），或为该平台补充 third_party/tarballs 下的静态包。")
    endif()
endif()

add_subdirectory(third_party)

# ---- apitab ------------------------------------------------------------------
# 官方 HuxerUI 消费方式：huxerui_add_app 生成 app 目标（C++20 + codegen + 资源集成），
# 领域层仍以 C++ 模块（FILE_SET）追加，普通 TU 通过 import 使用。
# SDK 解包必须在 find_package 之前（HuxerUI_DIR 指向解包产物的 CMake 包）。
include(cmake/apitab_vendor.cmake)
if(APPLE)
    apitab_extract(huxerui
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/huxerui-sdk-0.1.0-macos-arm64.tar.gz"
        "f1f3d115f0647d43e8149c82be1e628605fbef4414bf75bd1d7b9de7b1df0852"
        "huxerui-sdk-0.1.0-macos-arm64")
elseif(WIN32)
    apitab_extract(huxerui
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/huxerui-sdk-0.1.0-windows-x86_64.zip"
        "8d8863878a56b38ad1e669ba666297cc0526e896f3d5e32aabf8aeaaf0b68539"
        "huxerui-sdk-0.1.0-windows-x86_64")
else()
    apitab_extract(huxerui
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/huxerui-sdk-0.1.0-linux-x86_64.tar.gz"
        "147833204b7a08bdae28a2d66019679ac0814af2d3d7238c7a6ea6e39fb1c5a3"
        "huxerui-sdk-0.1.0-linux-x86_64")
endif()
set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root")
set(HuxerUI_DIR "${HUXERUI_SDK_DIR}/lib/cmake/HuxerUI" CACHE PATH "HuxerUI package" FORCE)
set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
find_package(HuxerUI CONFIG REQUIRED COMPONENTS shared)

set(APITAB_MODULE_UNITS
    # 引擎抽象与接口
    src/api_engine.cppm
    src/curl_engine.cppm
    src/k6_engine.cppm
    src/websocket_engine.cppm
    src/tcp_engine.cppm
    # 基础设施
    src/config.cppm
    src/db.cppm
    src/utils.cppm
    src/preferences.cppm
    # 领域 store（UI 无关）
    src/store/requests.cppm
    src/store/loadtest.cppm
    src/store/tcp.cppm
    src/store/websocket.cppm
)

huxerui_add_app(apitab
    SOURCES
        src/app_main.cpp
        src/ui/app.cpp
        src/ui/home_page.cpp
        src/ui/request_page.cpp
        src/ui/common.cpp
        # 模块实现单元（module apitab.X;）—— codegen 无 composable 标记时原样放行
        src/curl_engine.cpp
        src/k6_engine.cpp
        src/websocket_engine.cpp
        src/tcp_engine.cpp
        src/db.cpp
)

# 领域层 C++23 模块（.cppm 接口 + .cpp 实现单元；codegen 扫描会按扩展名跳过它们）
target_sources(apitab PRIVATE FILE_SET apitab_modules TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${APITAB_MODULE_UNITS})
set_target_properties(apitab PROPERTIES CXX_MODULE_STD ON)
target_compile_features(apitab PRIVATE cxx_std_23)
# 源码注释含中文：MSVC 必须显式 /utf-8，否则按 codepage 误读
if(MSVC)
    target_compile_options(apitab PRIVATE /utf-8)
endif()
target_include_directories(apitab PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(apitab PRIVATE
    asio
    ixwebsocket
    CURL::libcurl
    SQLiteCpp
    OpenSSL::SSL
    OpenSSL::Crypto
    apitab_json
    Threads::Threads
)

if(WIN32)
    # 版本信息 + 图标（从模板生成，版本号跟 project() 走）
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/apitab.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/apitab.rc" @ONLY)
    target_sources(apitab PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/apitab.rc")
    if(MSVC)
        target_link_options(apitab PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    elseif(MINGW)
        target_link_options(apitab PRIVATE -mwindows)
    endif()
endif()

# 图标资源放 <exe>/assets；HuxerUI 框架资源放 <exe>.resources/huxerui/
add_custom_command(TARGET apitab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:apitab>/assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:apitab>/apitab.resources/huxerui"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${HUXERUI_RESOURCES_BIN}" "$<TARGET_FILE_DIR:apitab>/apitab.resources/huxerui/resources.bin"
    COMMENT "Copying assets and HuxerUI resources next to apitab")
if(WIN32)
    # HuxerUI 运行时 DLL 需在 exe 旁
    add_custom_command(TARGET apitab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HUXERUI_SDK_DIR}/bin/huxerui.dll" "$<TARGET_FILE_DIR:apitab>"
        COMMENT "Copying huxerui.dll next to apitab")
endif()

# 仓库根的 compile_commands.json 软链（给 clangd 用；.gitignore 已忽略）
file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
                 "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
                 SYMBOLIC)

# ---- 测试（mcpp 时代被 app-main 的 main() 冲突挡住；CMake 下恢复）------------
# 独立于 CTest 的 BUILD_TESTING 变量：curl 等子项目会把它改成 OFF，不能让它
# 决定 apitab 自己的测试。
option(APITAB_BUILD_TESTS "Build apitab test executables" ON)
if(APITAB_BUILD_TESTS)
    enable_testing()
    add_executable(test_smoke tests/test_smoke.cpp)
    set_target_properties(test_smoke PROPERTIES CXX_MODULE_STD ON)
    target_compile_features(test_smoke PRIVATE cxx_std_23)
    add_test(NAME smoke COMMAND test_smoke)
endif()
