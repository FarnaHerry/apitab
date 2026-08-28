# CxxImportStdGate.cmake — enable CMake's experimental `import std` support.
#
# `import std;`（本仓库所有 apitab.* 模块都在用）在 CMake 里仍是 experimental
# feature：必须在 `project()` 之前把 CMAKE_EXPERIMENTAL_CXX_IMPORT_STD 设成
# 该 CMake 版本文档指定的 UUID，否则扫描器不认识 std 模块，配置直接失败。
# UUID 随 CMake 版本会变（见各版本源码 Help/dev/experimental.rst 的
# "C++ import std support" 一节）——升级 CMake 后如果 configure 报
# "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD is set to incorrect value"，去那里查新值。
#
# 已验证组合（见 git 历史）：CMake 4.4.2 + GCC 16.2.1 (libstdc++)。
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.1")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")
else()
    # 3.30 – 4.0
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "0e5b6991-d74f-4b3d-a41c-cf096e0b2508")
endif()
