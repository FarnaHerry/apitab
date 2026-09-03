# CMakeLists.txt — apitab 顶层构建（HuxerUI CLI 生成格式 + 本仓库 vendor 依赖）。
#
# 形状对齐 `huxerui create app` 产物：顶部项目自省块（CLI plan-only 配置读取）、
# platform/<平台>/main.cpp 平台入口、src/ 递归源码发现、resources/ 以 app 命名空间
# 注册。差异仅在于：依赖 vendor 在 third_party/（HuxerUI 优先 third_party/huxerui
# 源码编译，tarballs 里的预编译 SDK 兜底），领域层以 C++23 模块（FILE_SET）追加到同一 app 目标。
#
# 常用命令：
#   huxerui build linux [--profile debug|release]   # CLI 构建（.huxerui/build/linux/...）
#   huxerui run linux                               # CLI 构建并运行
#   cmake -B build -G Ninja && cmake --build build  # 直接 CMake 流程（仍然可用）
#   ctest --test-dir build                          # 冒烟测试
#   ./run.sh                                        # 运行 build/ 出的 GUI
cmake_minimum_required(VERSION 3.30)

# ---- HuxerUI CLI 项目自省 --------------------------------------------------
# `huxerui build/run/package` 先用 -DHUXERUI_PROJECT_PLAN_ONLY=ON 配置本项目，
# 读本块写出的 JSON 获知 kind/name/id/target；本块必须位于 project() 之前，
# 且不依赖任何语言/包解析（plan-only 配置会直接 return）。
if (HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
            "${HUXERUI_PROJECT_PLAN_OUTPUT}"
            DIRECTORY
    )
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "app",
  "name": "apitab",
  "id": "dev.farna.apitab",
  "target": "apitab"
}
]=])
endif ()
if (HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif ()

project(apitab VERSION 0.1.0 DESCRIPTION "API testing and load testing tool" LANGUAGES NONE)

# HUXERUI_LIBRARY_GRAPH_ONLY：CLI 扫描 huxerui 库依赖图时的特殊配置——
# 不启用语言、不构建，huxerui_add_app 只写出库图 JSON 即返回（见 SDK
# HuxerUIApp.cmake）。本项目的重活（第三方依赖/模块/链接）全部在该模式下跳过。
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # `import std` 的 experimental 门控必须在 enable_language(CXX) 前设置
    # （UUID 随 CMake 版本变，见 cmake/CxxImportStdGate.cmake 顶部注释）。
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    include(CxxImportStdGate)
    enable_language(C)
    enable_language(CXX)
    if (WIN32)
        enable_language(RC)
    endif ()

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)            # gnu++23：模块实现与 GCC 标准库组合最顺
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    # 只给用 import std 的目标开 CXX_MODULE_STD（apitab / test_smoke），
    # 不影响 curl 等 C++17 子项目。
    set(CMAKE_CXX_MODULE_STD OFF)

    # 默认 Release（单配置生成器）；调试请显式 -DCMAKE_BUILD_TYPE=Debug
    # （huxerui CLI 会按 --profile 显式传入，不受此默认影响）
    if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif ()

    find_package(Threads REQUIRED)
endif ()

# ---- HuxerUI（0.2.0 SDK / 源码双通道，HUXERUI_HOME 契约）-------------------
# 优先级（P1-B1.0 固定顺序，见 docs/plans/island-structure-theme.md §14.4）：
#   1. 显式 HUXERUI_HOME 源码（有 CMakeLists.txt + include/huxerui/huxerui.h）。
#   2. third_party/huxerui 源码（本地 git clone 的上游仓库，add_subdirectory 编译，
#      跟踪上游 `git pull` 即可；不入库，.gitignore 已忽略）。
#      CLI 自动注入的已安装 SDK 前缀（HUXERUI_HOME 指向 SDK）不得越过前两级。
#   3. 已安装 SDK（HUXERUI_HOME 指向含 lib/cmake/HuxerUI 的 SDK 目录）。
#   4. 仓库离线 SDK（tarball，Linux 专用；macOS/Windows 不提供离线包）。
#
# APITAB_HUXERUI_FORCE_SDK=ON：唯一明确跳过源码的开关，只走第 3/4 通道（发布门禁
# 与兼容测试用）。源码模式编译 Linux 后端需要 gtk4-devel libsoup3-devel。
# ComboBox 等新 API 允许在默认源码通道使用；进入合并/发布验收前必须刷新可消费 SDK，
# 确保 APITAB_HUXERUI_FORCE_SDK=ON 恢复通过（见 island-structure-theme.md §14.2）。
include(cmake/apitab_vendor.cmake)
set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI 0.2.0 SDK or source directory")
set(APITAB_HUXERUI_BUNDLED_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui" CACHE PATH "Bundled HuxerUI source checkout (override for testing to simulate missing source without moving shared directory)")
set(APITAB_HUXERUI_SOURCE_DIR "")
option(APITAB_HUXERUI_FORCE_SDK "Skip source checkout; use SDK fallback only" OFF)
option(APITAB_HUXERUI_MOCK_MISSING_DEPS "Force missing gtk4/libsoup for selection-matrix testing" OFF)
set(APITAB_HUXERUI_FROM_SOURCE OFF)

# 优先级 1：显式 HUXERUI_HOME 源码（用户显式指向源码目录时优先于仓库内源码）
if (NOT APITAB_HUXERUI_FORCE_SDK
        AND HUXERUI_HOME
        AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
        AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
    set(APITAB_HUXERUI_SOURCE_DIR "${HUXERUI_HOME}")
endif ()

# 优先级 2：仓库内 third_party/huxerui 源码（默认 source-first）
if (NOT APITAB_HUXERUI_SOURCE_DIR
        AND NOT APITAB_HUXERUI_FORCE_SDK
        AND EXISTS "${APITAB_HUXERUI_BUNDLED_SOURCE_DIR}/CMakeLists.txt"
        AND EXISTS "${APITAB_HUXERUI_BUNDLED_SOURCE_DIR}/include/huxerui/huxerui.h")
    set(APITAB_HUXERUI_SOURCE_DIR "${APITAB_HUXERUI_BUNDLED_SOURCE_DIR}")
endif ()

# 源码依赖检查（Linux）：缺 gtk4/libsoup-3.0 开发包时醒目回落 SDK
if (APITAB_HUXERUI_SOURCE_DIR
        AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
        AND NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (APITAB_HUXERUI_MOCK_MISSING_DEPS)
        set(APITAB_HUXERUI_GTK4_FOUND OFF)
        set(APITAB_HUXERUI_LIBSOUP_FOUND OFF)
    else ()
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(APITAB_HUXERUI_GTK4 QUIET gtk4>=4.6)
            pkg_check_modules(APITAB_HUXERUI_LIBSOUP QUIET libsoup-3.0>=3.0)
        endif ()
    endif ()
    if (NOT APITAB_HUXERUI_GTK4_FOUND OR NOT APITAB_HUXERUI_LIBSOUP_FOUND)
        message(WARNING "apitab: ${APITAB_HUXERUI_SOURCE_DIR} 存在但缺 gtk4/libsoup-3.0 开发包"
            "（sudo dnf install gtk4-devel libsoup3-devel），回落预编译 SDK —— 本次未使用源码")
        set(APITAB_HUXERUI_SOURCE_DIR "")
    endif ()
endif ()

# 源码模式
if (APITAB_HUXERUI_SOURCE_DIR)
    set(APITAB_HUXERUI_FROM_SOURCE ON)
    # 上游顶层作为子项目时 tests/examples/CLI 默认 OFF；static 切片必须关掉——
    # _huxerui_select_framework_target 偏好 static，留着会让 app 链到静态库。
    set(HUXERUI_BUILD_STATIC OFF)
    # 上游预置宿主工具可能落后于源码（4a56daf 更新了 svg/path 编码却只发了
    # mac/windows/android 的预置 hrc，linux 仍是旧的 → 新运行时解旧资源包直接
    # 抛 "unknown path operation"）。本地若有用当前源码编出的工具
    # （third_party/huxerui-tools/<platform>/<arch>/{hcg,hrc}，构建方法见
    # CLAUDE.md），优先使用；缺失时回落上游预置。
    set(APITAB_HUXERUI_LOCAL_TOOLS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-tools")
    if (APITAB_HUXERUI_SOURCE_DIR STREQUAL APITAB_HUXERUI_BUNDLED_SOURCE_DIR
            AND EXISTS "${APITAB_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hrc"
            AND EXISTS "${APITAB_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hcg")
        set(HUXERUI_HOST_TOOL_ROOT "${APITAB_HUXERUI_LOCAL_TOOLS}")
        message(STATUS "apitab: 宿主工具用本地源码构建（third_party/huxerui-tools）")
    endif ()
    add_subdirectory("${APITAB_HUXERUI_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/huxerui-sdk"
        EXCLUDE_FROM_ALL)
    # HuxerUI 0.2 currently attaches -fobjc-arc to the whole macOS object
    # target. That works with AppleClang but breaks the GCC C++ compiler used
    # for C++23 `import std`; retain ARC only for Objective-C++ sources.
    if (APPLE AND TARGET huxerui_core_objects)
        get_target_property(APITAB_HUXERUI_COMPILE_OPTIONS
            huxerui_core_objects COMPILE_OPTIONS)
        if (APITAB_HUXERUI_COMPILE_OPTIONS)
            list(REMOVE_ITEM APITAB_HUXERUI_COMPILE_OPTIONS -fobjc-arc)
            list(APPEND APITAB_HUXERUI_COMPILE_OPTIONS
                "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
            set_target_properties(huxerui_core_objects PROPERTIES
                COMPILE_OPTIONS "${APITAB_HUXERUI_COMPILE_OPTIONS}")
        endif ()
    endif ()
    # 打印实际来源与 commit，使 `huxerui build/run` 默认输出可见（P1-B1.0 要求）
    execute_process(COMMAND git -C "${APITAB_HUXERUI_SOURCE_DIR}" rev-parse --short HEAD
        OUTPUT_VARIABLE APITAB_HUXERUI_SOURCE_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (APITAB_HUXERUI_SOURCE_COMMIT)
        message(STATUS "apitab: HuxerUI 源码编译（${APITAB_HUXERUI_SOURCE_DIR} @ ${APITAB_HUXERUI_SOURCE_COMMIT}）")
    else ()
        message(STATUS "apitab: HuxerUI 源码编译（${APITAB_HUXERUI_SOURCE_DIR}）")
    endif ()
else ()
    # SDK 模式：优先级 3（HUXERUI_HOME SDK）→ 优先级 4（离线 tarball，Linux 专用）
    # find_package 不依赖语言启用，graph-only 模式照常执行。
    if (HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/lib/cmake/HuxerUI/HuxerUIConfig.cmake")
        set(HUXERUI_SDK_DIR "${HUXERUI_HOME}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
        message(STATUS "apitab: HuxerUI SDK（已安装，${HUXERUI_SDK_DIR}）")
    elseif (APPLE OR WIN32)
        message(FATAL_ERROR
            "apitab: macOS/Windows 未提供离线 SDK 包；"
            "请用 HUXERUI_HOME 指向 0.2.0 源码或已安装 SDK，"
            "或 clone third_party/huxerui 后默认以源码模式编译")
    else ()
        apitab_extract(huxerui
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/huxerui-sdk-0.2.0-linux-x86_64.tar.gz"
            "e184a62a0e5e22cd71835d5278034d65f8a30a2c48273513633f88719a1243e5"
            "huxerui-sdk-0.2.0-linux-x86_64")
        set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
        message(STATUS "apitab: HuxerUI SDK（离线 tarball，${HUXERUI_SDK_DIR}）")
    endif ()
    if (NOT HUXERUI_SDK_DIR)
        set(HUXERUI_SDK_DIR "${huxerui_SOURCE_DIR}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
    endif ()
    set(HuxerUI_DIR "${HUXERUI_SDK_DIR}/lib/cmake/HuxerUI" CACHE PATH "HuxerUI package" FORCE)
    set(HUXERUI_RESOURCES_BIN "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
    # shared 组件：桌面 Linux/Windows 运行时动态库（static 切片在 Linux 要 gtk4
    # 开发包，本仓库不用）。
    find_package(HuxerUI 0.2.0 CONFIG REQUIRED COMPONENTS shared
        PATHS "${HUXERUI_SDK_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif ()

# 平台壳的 HuxerUI 配置钩子（生成格式：platform/<平台>/huxerui.cmake 可选）
if (WIN32)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/huxerui.cmake" OPTIONAL)
elseif (APPLE AND NOT IOS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/huxerui.cmake" OPTIONAL)
endif ()

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # ---- OpenSSL 解析（必须在 add_subdirectory(third_party) 前：imported target
    # 要对 curl 等子项目全局可见）----------------------------------------
    # 优先系统 OpenSSL（CI: ubuntu libssl-dev / macos brew openssl@3 / windows
    # chocolatey）；系统没有时，linux x86_64 使用 vendor 的静态 3.5.1。
    find_package(OpenSSL QUIET)
    if (NOT OpenSSL_FOUND)
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
            apitab_extract(openssl
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tarballs/openssl-3.5.1-linux-x86_64.tar.gz"
                "d749ee7ec6bda301dba4a67dc7196d4a04b310f664dc360d6ab2e49298b731cf"
                "openssl-3.5.1-linux-x86_64")
            set(OPENSSL_ROOT_DIR "${openssl_SOURCE_DIR}" CACHE PATH "vendored OpenSSL" FORCE)
            set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
            find_package(OpenSSL REQUIRED)
            message(STATUS "apitab: using vendored OpenSSL ${OPENSSL_VERSION} (static)")
        else ()
            message(FATAL_ERROR
                "未找到 OpenSSL 开发头文件/库。请安装系统 OpenSSL（libssl-dev / "
                "openssl@3 / vcpkg openssl），或为该平台补充 third_party/tarballs 下的静态包。")
        endif ()
    endif ()

    add_subdirectory(third_party)
endif ()

# ---- apitab app 目标 -------------------------------------------------------
# 源码发现对齐生成格式：src/ 下递归 glob 普通 C++ 源（UI composable 源 + 领域
# 模块的实现单元 .cpp；codegen 无 composable 标记时原样放行）。模块接口 .cppm
# 不经 glob，由 FILE_SET 显式追加。
file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx"
)
# 平台入口（graph-only 配置不编译任何东西，跳过）
if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if (WIN32)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/windows/main.cpp")
    elseif (APPLE AND NOT IOS)
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/main.cpp")
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/platform/linux/main.cpp")
    endif ()
endif ()

# CLI 依赖图输出位置（生成格式约定；huxerui_add_app 在存在该变量时写出）
set(HUXERUI_LIBRARY_GRAPH_OUTPUT
        "${CMAKE_CURRENT_SOURCE_DIR}/.huxerui/generated/libraries.json"
)

set(APITAB_MODULE_UNITS
    # 引擎抽象与接口
    src/api_engine.cppm
    src/curl_engine.cppm
    src/k6_engine.cppm
    # 基础设施
    src/config.cppm
    src/db.cppm
    src/utils.cppm
    src/preferences.cppm
    # 领域 store（UI 无关）
    src/store/requests.cppm
    src/store/loadtest.cppm
)

huxerui_add_app(apitab
    SOURCES
        ${APP_SOURCE_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    RESOURCE_NAMESPACE app
    BUNDLE_NAME "apitab"
    BUNDLE_IDENTIFIER "dev.farna.apitab"
)

# P1-B1.1 ComboBox 能力检测（937efb1 源码有，0.2.0 SDK 无）——决定是否编译 ComboBox 分支
set(APITAB_HAS_COMBOBOX OFF)
if (APITAB_HUXERUI_FROM_SOURCE AND EXISTS "${APITAB_HUXERUI_SOURCE_DIR}/include/huxerui/view.h")
    file(READ "${APITAB_HUXERUI_SOURCE_DIR}/include/huxerui/view.h" _apitab_view_h)
    if (_apitab_view_h MATCHES "class ComboBox")
        set(APITAB_HAS_COMBOBOX ON)
    endif()
elseif (HUXERUI_SDK_DIR AND EXISTS "${HUXERUI_SDK_DIR}/include/huxerui/view.h")
    file(READ "${HUXERUI_SDK_DIR}/include/huxerui/view.h" _apitab_view_h)
    if (_apitab_view_h MATCHES "class ComboBox")
        set(APITAB_HAS_COMBOBOX ON)
    endif()
endif()
if (APITAB_HAS_COMBOBOX)
    message(STATUS "apitab: ComboBox available（源码通道）")
endif()

# ---- CodeEditor（SweetEditor 代码编辑器组件库，HuxerUI 依赖库）---------------
# huxerui_use_library 官方机制：add_subdirectory + 链接 + 资源包合并进 app 包。
# HuxerUI 目标已存在（源码或 SDK 模式都一样），sweetedit 不再自行获取 HuxerUI；
# HUXERUI_HOME 仅供它做新版钩子（ShouldCommitFocusOnPointerUp）的特性检测。
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-sweetedit/CMakeLists.txt")
    if (APITAB_HUXERUI_FROM_SOURCE)
        set(HUXERUI_HOME "${APITAB_HUXERUI_SOURCE_DIR}"
            CACHE PATH "HuxerUI source dir (sweetedit feature detection)" FORCE)
    elseif (HUXERUI_SDK_DIR)
        set(HUXERUI_HOME "${HUXERUI_SDK_DIR}"
            CACHE PATH "HuxerUI SDK dir (sweetedit feature detection)" FORCE)
    endif ()
    huxerui_use_library(apitab TARGET codeeditor
        PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-sweetedit")

    # CodeEditor keeps syntax analysis optional. Reuse its upstream-maintained
    # SweetLine provider so JSON/XML/GraphQL/JavaScript editors retain syntax
    # colors, folding, indent guides, bracket matching, and incremental updates.
    if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
        set(SWEETLINE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(SWEETLINE_BUILD_STATIC ON CACHE BOOL "" FORCE)
        set(SWEETLINE_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(SWEETLINE_BUILD_ANDROID_JNI OFF CACHE BOOL "" FORCE)
        set(SWEETLINE_BUILD_WASM_EMBIND OFF CACHE BOOL "" FORCE)
        set(SWEETLINE_BUILD_APPLE_FRAMEWORK OFF CACHE BOOL "" FORCE)
        add_subdirectory(
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-sweetedit/3dparty/SweetLine"
            "${CMAKE_BINARY_DIR}/sweetline"
            EXCLUDE_FROM_ALL
        )
        target_sources(apitab PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-sweetedit/examples/preview/src/sweetline_provider.cpp"
        )
        target_include_directories(apitab PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-sweetedit/examples/preview/src"
        )
        target_link_libraries(apitab PRIVATE SweetLine::sweetline_static)
    endif ()
endif ()

if (NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # 领域层 C++23 模块（.cppm 接口；对应 .cpp 实现单元已随 glob 进 SOURCES）
    target_sources(apitab PRIVATE FILE_SET apitab_modules TYPE CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${APITAB_MODULE_UNITS})
    set_target_properties(apitab PROPERTIES CXX_MODULE_STD ON)
    # 显式打开全源扫描：模块实现单元（普通 .cpp 里的 module apitab.*;）必须经
    # 扫描拿到 -fmodules-ts。SDK 模式下 CMP0155 默认行为恰好扫描全部源，源码
    # 模式（add_subdirectory huxerui）下该默认不覆盖普通 .cpp，会丢模块标志。
    set_target_properties(apitab PROPERTIES CXX_SCAN_FOR_MODULES ON)
    target_compile_features(apitab PRIVATE cxx_std_23)
    # 应用版本编译期常量（设置页“关于”展示）：唯一来源是顶层 project(VERSION)。
    target_compile_definitions(apitab PRIVATE APITAB_VERSION="${PROJECT_VERSION}")
    if (APITAB_HAS_COMBOBOX)
        target_compile_definitions(apitab PRIVATE APITAB_HAS_COMBOBOX)
    endif()
    # windows.h 的 min/max 宏会毁掉所有经 ixwebsocket/asio/curl 头链引入它的
    # TU 里的 std::max/std::min（MSVC C2589）；宏不跨 TU，必须整个目标定义
    # （config.cppm 等文件内的 #ifndef 守卫与此兼容）。
    if (WIN32)
        target_compile_definitions(apitab PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif ()
    # 源码注释含中文：MSVC 必须显式 /utf-8，否则按 codepage 误读
    if (MSVC)
        target_compile_options(apitab PRIVATE /utf-8)
    endif ()
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

    # 生成格式钩子：平台清单 / Info.plist
    if (WIN32 AND HUXERUI_WINDOWS_MANIFEST)
        target_sources(apitab PRIVATE "${HUXERUI_WINDOWS_MANIFEST}")
    endif ()
    if (APPLE AND NOT IOS AND HUXERUI_MACOS_INFO_PLIST)
        set_target_properties(apitab PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${HUXERUI_MACOS_INFO_PLIST}")
    endif ()

    if (WIN32)
        # 版本信息 + 图标（从模板生成，版本号跟 project() 走）
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/apitab.rc.in"
                       "${CMAKE_CURRENT_BINARY_DIR}/apitab.rc" @ONLY)
        target_sources(apitab PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/apitab.rc")
        if (MSVC)
            target_link_options(apitab PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
        elseif (MINGW)
            target_link_options(apitab PRIVATE -mwindows)
        endif ()
    endif ()

    # 图标资源放 <exe>/assets；HuxerUI 资源包（内置+app 合并，由 huxerui_add_app 的
    # 资源构建生成）放 <exe>.resources/huxerui/resources.bin —— 运行时按此路径查找。
    add_custom_command(TARGET apitab POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:apitab>/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:apitab>/apitab.resources/huxerui"
        COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_BINARY_DIR}/huxerui-resources/apitab/package/huxerui/resources.bin"
                "$<TARGET_FILE_DIR:apitab>/apitab.resources/huxerui/resources.bin"
        COMMENT "Copying assets and the merged HuxerUI resource package next to apitab")
    if (WIN32)
        # HuxerUI 运行时 DLL 需在 exe 旁。源码通道没有 HUXERUI_SDK_DIR，且
        # dll 实际落在构建树 RUNTIME 输出（多配置时还带 config 子目录），
        # 只有 $<TARGET_FILE:> 能跨两种通道、跨生成器解析；目标缺失
        # （如静态框架构建）时回落 SDK 路径，两处都没有就跳过。
        set(APITAB_HUXERUI_DLL "")
        if (TARGET HuxerUI::huxerui)
            get_target_property(APITAB_HUXERUI_DLL_TYPE HuxerUI::huxerui TYPE)
            if (APITAB_HUXERUI_DLL_TYPE STREQUAL "SHARED_LIBRARY")
                set(APITAB_HUXERUI_DLL "$<TARGET_FILE:HuxerUI::huxerui>")
            endif ()
        endif ()
        if (NOT APITAB_HUXERUI_DLL AND HUXERUI_SDK_DIR
                AND EXISTS "${HUXERUI_SDK_DIR}/bin/huxerui.dll")
            set(APITAB_HUXERUI_DLL "${HUXERUI_SDK_DIR}/bin/huxerui.dll")
        endif ()
        if (APITAB_HUXERUI_DLL)
            add_custom_command(TARGET apitab POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${APITAB_HUXERUI_DLL}" "$<TARGET_FILE_DIR:apitab>"
                COMMENT "Copying huxerui.dll next to apitab")
        endif ()
    endif ()

    # 仓库根的 compile_commands.json 软链（给 clangd 用；.gitignore 已忽略）
    # 并行测试/多构建并发时多个 build 目录同时创建同一软链接会竞态；已存在则跳过
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json")
        file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
                         "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json"
                         SYMBOLIC)
    endif()

    # ---- 测试 ---------------------------------------------------------------
    # 独立于 CTest 的 BUILD_TESTING 变量：curl 等子项目会把它改成 OFF，不能让它
    # 决定 apitab 自己的测试。
    option(APITAB_BUILD_TESTS "Build apitab test executables" ON)
    if (APITAB_BUILD_TESTS)
        enable_testing()
        add_executable(test_smoke tests/test_smoke.cpp)
        set_target_properties(test_smoke PROPERTIES CXX_MODULE_STD ON)
        target_compile_features(test_smoke PRIVATE cxx_std_23)
        add_test(NAME smoke COMMAND test_smoke)
        # P1-B1.0 构建选择矩阵（5 场景，跑 5 次 cmake 配置，约 10s）。
        # 日常 `ctest` 会执行；CI 三通道验证见 P1-B1.0a。
        add_test(NAME huxerui_selection_matrix
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/huxerui_selection_matrix.sh")
        set_tests_properties(huxerui_selection_matrix PROPERTIES TIMEOUT 120)
    endif ()
endif ()
