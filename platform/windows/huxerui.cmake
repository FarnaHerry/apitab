set(HUXERUI_WINDOWS_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/app.manifest")
set(HUXERUI_WINDOWS_RESOURCE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/huxerui-platform/windows")
file(MAKE_DIRECTORY "${HUXERUI_WINDOWS_RESOURCE_DIRECTORY}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/../../cmake/apitab.rc.in"
    "${HUXERUI_WINDOWS_RESOURCE_DIRECTORY}/app.rc"
    @ONLY)
set(HUXERUI_WINDOWS_RESOURCE
    "${HUXERUI_WINDOWS_RESOURCE_DIRECTORY}/app.rc")

# `huxerui package windows` sets HUXERUI_PACKAGE and invokes this hook after
# the normal application target has been configured. Burn owns elevation,
# rollback, repair, and uninstall; this project supplies its HuxerUI interface.
function(huxerui_configure_windows_project_package target_name install_component)
    if (NOT HUXERUI_PACKAGE)
        return()
    endif ()
    if (NOT COMMAND huxerui_add_windows_installer)
        message(FATAL_ERROR "apitab: current HuxerUI SDK has no Windows installer support")
    endif ()

    install(TARGETS ${target_name} RUNTIME DESTINATION . COMPONENT "${install_component}")
    get_target_property(_apitab_resources ${target_name} HUXERUI_RESOURCE_PACKAGE)
    if (_apitab_resources AND NOT _apitab_resources MATCHES "-NOTFOUND$")
        install(DIRECTORY "${_apitab_resources}/" DESTINATION "${target_name}.resources"
            COMPONENT "${install_component}")
    endif ()
    install(DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../assets/" DESTINATION assets
        COMPONENT "${install_component}")

    # HuxerUI packages only declared install rules; do not rely on adjacent
    # build outputs that happen to exist during a normal desktop build.
    if (TARGET HuxerUI::huxerui)
        get_target_property(_apitab_hui_type HuxerUI::huxerui TYPE)
        if (_apitab_hui_type STREQUAL "SHARED_LIBRARY")
            install(FILES "$<TARGET_FILE:HuxerUI::huxerui>" DESTINATION . COMPONENT "${install_component}")
        endif ()
    endif ()
    if (OPENSSL_INCLUDE_DIR)
        get_filename_component(_apitab_openssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
        file(GLOB _apitab_openssl_dlls "${_apitab_openssl_root}/bin/libssl-*.dll"
            "${_apitab_openssl_root}/bin/libcrypto-*.dll")
        if (_apitab_openssl_dlls)
            install(FILES ${_apitab_openssl_dlls} DESTINATION . COMPONENT "${install_component}")
        endif ()
    endif ()
    if (_apitab_crt_dir)
        install(FILES "${_apitab_crt_dir}/msvcp140.dll" "${_apitab_crt_dir}/vcruntime140.dll"
            "${_apitab_crt_dir}/vcruntime140_1.dll" DESTINATION . COMPONENT "${install_component}")
    endif ()

    # CI downloads the pinned k6 binary before calling `huxerui package`.
    set(_apitab_k6 "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../engines/k6.exe")
    if (NOT EXISTS "${_apitab_k6}")
        message(FATAL_ERROR "apitab: Windows packaging requires engines/k6.exe")
    endif ()
    install(PROGRAMS "${_apitab_k6}" DESTINATION engines COMPONENT "${install_component}")

    string(UUID _apitab_msi_upgrade_code NAMESPACE 6ba7b810-9dad-11d1-80b4-00c04fd430c8
        NAME "dev.farna.apitab.msi" TYPE SHA1 UPPER)
    string(UUID _apitab_bundle_upgrade_code NAMESPACE 6ba7b810-9dad-11d1-80b4-00c04fd430c8
        NAME "dev.farna.apitab.bundle" TYPE SHA1 UPPER)
    set(APITAB_WINDOWS_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/huxerui-package/windows")
    set(APITAB_WINDOWS_VERSION "${PROJECT_VERSION}")
    file(MAKE_DIRECTORY "${APITAB_WINDOWS_PACKAGE_DIR}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/Package.wxs.in"
        "${APITAB_WINDOWS_PACKAGE_DIR}/Package.wxs" @ONLY)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/Bundle.wxs.in"
        "${APITAB_WINDOWS_PACKAGE_DIR}/Bundle.wxs" @ONLY)

    file(GLOB_RECURSE _apitab_installer_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/src/*.cpp")
    huxerui_add_windows_installer(${target_name}_installer
        SOURCES
            ${_apitab_installer_sources}
            "${HUXERUI_WINDOWS_RESOURCE}"
        RESOURCES
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/package/resources"
        RESOURCE_NAMESPACE apitab_installer
        INTEGRATION_OUTPUT "${APITAB_WINDOWS_PACKAGE_DIR}/$<CONFIG>/installer.json")
    set_target_properties(${target_name}_installer PROPERTIES OUTPUT_NAME "apitab-Installer")
    file(GENERATE OUTPUT "${APITAB_WINDOWS_PACKAGE_DIR}/$<CONFIG>/package.json"
        CONTENT "{\n  \"schema\": 1,\n  \"name\": \"apitab\",\n  \"target\": \"apitab\",\n  \"version\": \"${PROJECT_VERSION}\",\n  \"installComponent\": \"${install_component}\",\n  \"packageSource\": \"${APITAB_WINDOWS_PACKAGE_DIR}/Package.wxs\",\n  \"bundleSource\": \"${APITAB_WINDOWS_PACKAGE_DIR}/Bundle.wxs\",\n  \"installerPlan\": \"${APITAB_WINDOWS_PACKAGE_DIR}/$<CONFIG>/installer.json\"\n}\n")
endfunction()
