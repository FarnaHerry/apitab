# CMakeLists.txt — apitab 顶层构建（自 mcpp 迁移，功能等价：见 docs/ 下迁移说明）。
#
# 形状：src/ 是全模块化 C++23（import std + apitab.* 命名模块），主入口
# main() 来自 EUI-NEO 的 glfw_app_main.cpp（旧 mcpp 的 app-main feature），
# 这里作为普通源文件编进 apitab 目标。依赖全部 vendor 在 third_party/。
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
# 模块单元清单（显式列出，不用 GLOB）。
# *.cppm 是接口单元，进 CXX_MODULES 文件集；四个 engine 的 .cpp 与 db.cpp 是
# 实现单元（`module apitab.X;` 声明在文件内），CMake 要求它们放在普通源列表，
# 扫描器会自动接上主接口的 BMI 依赖。
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
    src/i18n.cppm
    # 领域 store（UI 无关）
    src/store/requests.cppm
    src/store/loadtest.cppm
    src/store/tcp.cppm
    src/store/websocket.cppm
    # HuxerUI 前端
    src/store/ui.cppm
    src/ui/common.cppm
    src/ui/home_page.cppm
    src/ui/request_page.cppm
    src/ui/app.cppm
)

add_executable(apitab
    src/app_main.cpp
    # 模块实现单元（module apitab.X;）
    src/curl_engine.cpp
    src/k6_engine.cpp
    src/websocket_engine.cpp
    src/tcp_engine.cpp
    src/db.cpp
)
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
    huxerui
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
