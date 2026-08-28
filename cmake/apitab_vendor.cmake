# apitab_vendor.cmake — vendor 解包助手。
# 被 顶层 CMakeLists（OpenSSL 解析需要全局 imported target）与
# third_party/CMakeLists 共同 include；重复 include 无害。
set(APITAB_VENDOR_DIR "${CMAKE_BINARY_DIR}/vendor")
file(MAKE_DIRECTORY "${APITAB_VENDOR_DIR}")

# apitab_extract(<name> <archive> <sha256> <topdir>)
# 解包（带 sha 记录，tarball 变更时自动重解）；topdir 的路径存到 <name>_SOURCE_DIR。
function(apitab_extract name archive sha256 topdir)
    set(dest "${APITAB_VENDOR_DIR}/${name}")
    set(recorded "")
    if(EXISTS "${dest}/.vendor-sha256")
        file(READ "${dest}/.vendor-sha256" recorded)
    endif()
    string(STRIP "${recorded}" recorded)
    if(NOT recorded STREQUAL sha256 OR NOT EXISTS "${dest}/${topdir}")
        message(STATUS "apitab vendor: extracting ${archive} -> ${dest}")
        file(REMOVE_RECURSE "${dest}")
        file(MAKE_DIRECTORY "${dest}")
        # CMake 4.0 起 ARCHIVE_EXTRACT 改为 INPUT/DESTINATION 关键字形式，
        # 位置参数形式在 4.0+ 直接报错；3.x 仍只认旧形式。
        if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dest}")
        else()
            file(ARCHIVE_EXTRACT "${archive}" TO "${dest}")
        endif()
        if(NOT EXISTS "${dest}/${topdir}")
            message(FATAL_ERROR "apitab vendor: ${archive} 里找不到 ${topdir}")
        endif()
        file(WRITE "${dest}/.vendor-sha256" "${sha256}")
    endif()
    set(${name}_SOURCE_DIR "${dest}/${topdir}" PARENT_SCOPE)
endfunction()
