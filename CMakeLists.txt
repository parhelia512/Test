cmake_minimum_required(VERSION 4.4)

if(HUXERUI_PROJECT_PLAN_OUTPUT)
    get_filename_component(HUXERUI_PROJECT_PLAN_DIRECTORY
        "${HUXERUI_PROJECT_PLAN_OUTPUT}" DIRECTORY)
    file(MAKE_DIRECTORY "${HUXERUI_PROJECT_PLAN_DIRECTORY}")
    file(WRITE "${HUXERUI_PROJECT_PLAN_OUTPUT}" [=[
{
  "schema": 1,
  "kind": "app",
  "name": "clash-flux",
  "id": "dev.farna.clashflux",
  "target": "clash-flux"
}
]=])
endif()
if(HUXERUI_PROJECT_PLAN_ONLY)
    return()
endif()

project(clash-flux VERSION 0.3.100
    DESCRIPTION "Mobile sing-box proxy client" LANGUAGES NONE)

if(NOT HUXERUI_LIBRARY_GRAPH_ONLY AND NOT ANDROID AND NOT IOS)
    message(FATAL_ERROR "Clash-Flux is mobile-only. Configure Android or iOS.")
endif()

if(NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    include(CxxImportStdGate)
    enable_language(C)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS ON)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(CMAKE_CXX_MODULE_STD OFF)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
    find_package(Threads REQUIRED)
    find_package(ZLIB QUIET)
endif()

# Use the same pinned HuxerUI revision for native code and Android Java modules.
include(cmake/clashflux_vendor.cmake)
set(HUXERUI_HOME "$ENV{HUXERUI_HOME}" CACHE PATH "HuxerUI SDK or pinned source directory")
set(CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui" CACHE PATH "Pinned HuxerUI source")
set(CLASHFLUX_HUXERUI_SOURCE_DIR "")
if(HUXERUI_HOME AND EXISTS "${HUXERUI_HOME}/CMakeLists.txt"
        AND EXISTS "${HUXERUI_HOME}/include/huxerui/huxerui.h")
    set(CLASHFLUX_HUXERUI_SOURCE_DIR "${HUXERUI_HOME}")
elseif(EXISTS "${CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR}/CMakeLists.txt"
        AND EXISTS "${CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR}/include/huxerui/huxerui.h")
    set(CLASHFLUX_HUXERUI_SOURCE_DIR "${CLASHFLUX_HUXERUI_BUNDLED_SOURCE_DIR}")
endif()

if(CLASHFLUX_HUXERUI_SOURCE_DIR)
    set(CLASHFLUX_HUXERUI_FROM_SOURCE ON)
    if(IOS)
        set(HUXERUI_BUILD_SHARED OFF)
        set(HUXERUI_BUILD_STATIC ON)
    else()
        set(HUXERUI_BUILD_STATIC OFF)
    endif()
    set(CLASHFLUX_HUXERUI_LOCAL_TOOLS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/huxerui-tools")
    if(EXISTS "${CLASHFLUX_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hrc"
            AND EXISTS "${CLASHFLUX_HUXERUI_LOCAL_TOOLS}/linux/x86_64/hcg")
        set(HUXERUI_HOST_TOOL_ROOT "${CLASHFLUX_HUXERUI_LOCAL_TOOLS}")
    endif()
    add_subdirectory("${CLASHFLUX_HUXERUI_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}/huxerui-sdk" EXCLUDE_FROM_ALL)
else()
    if(NOT HUXERUI_HOME OR NOT EXISTS "${HUXERUI_HOME}/lib/cmake/HuxerUI/HuxerUIConfig.cmake")
        message(FATAL_ERROR "Set HUXERUI_HOME to the pinned HuxerUI source or a mobile SDK")
    endif()
    set(HUXERUI_SDK_DIR "${HUXERUI_HOME}" CACHE INTERNAL "HuxerUI SDK root" FORCE)
    set(HuxerUI_DIR "${HUXERUI_SDK_DIR}/lib/cmake/HuxerUI" CACHE PATH "HuxerUI package" FORCE)
    set(HUXERUI_RESOURCES_BIN
        "${HUXERUI_SDK_DIR}/share/huxerui/resources/huxerui/resources.bin" CACHE INTERNAL "")
    find_package(HuxerUI 0.3.0 CONFIG REQUIRED COMPONENTS shared
        PATHS "${HUXERUI_SDK_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()

if(NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    # Android/iOS downloads use the platform-native HTTPS implementations.
    add_subdirectory(third_party)
endif()

if(ANDROID)
    include(cmake/AndroidLegacy.cmake)
    clashflux_prepare_android_sources(
        "${CMAKE_CURRENT_BINARY_DIR}/android-legacy"
        CLASHFLUX_ANDROID_LEGACY_HEADER
        CLASHFLUX_ANDROID_LEGACY_CORE
        CLASHFLUX_ANDROID_LEGACY_UI)
    set(APP_SOURCE_FILES src/app.cpp
        "${CLASHFLUX_ANDROID_LEGACY_CORE}"
        src/profile_link.cpp
        src/ui/qr_photo_decoder.cpp
        src/ui/app_update_package.cpp
        src/ui/android_update_installer.cpp
        platform/android/android_bridge.cpp
        ${CLASHFLUX_ANDROID_LEGACY_UI})
else()
    file(GLOB_RECURSE APP_SOURCE_FILES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    list(REMOVE_ITEM APP_SOURCE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/src/wire_codec.cpp")
endif()

set(HUXERUI_LIBRARY_GRAPH_OUTPUT
    "${CMAKE_CURRENT_SOURCE_DIR}/.huxerui/generated/libraries.json")

set(CLASHFLUX_MODULE_UNITS
    src/config.cppm
    src/utils.cppm
    src/netinfo.cppm
    src/model.cppm
    src/db.cppm
    src/vpn.cppm
    src/pptp.cppm
    src/openvpn.cppm
    src/core.cppm
    src/singbox.cppm
    src/rule_provider_cache.cppm
    src/proxy_provider_cache.cppm
    src/provider_resources.cppm
    src/routing.cppm
    src/api.cppm
    src/stream.cppm
    src/store/core_store.cppm
    src/store/persistence.cppm
    src/store/vpn.cppm
    src/store/profiles.cppm)

set(CLASHFLUX_APP_RESOURCE_OPTIONS)
if(ANDROID AND HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT)
    list(APPEND CLASHFLUX_APP_RESOURCE_OPTIONS RESOURCE_OUTPUT_DIRECTORY
        "${HUXERUI_ANDROID_RESOURCE_OUTPUT_ROOT}/${ANDROID_ABI}")
endif()

huxerui_add_app(clash-flux
    SOURCES ${APP_SOURCE_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    RESOURCE_NAMESPACE app
    BUNDLE_NAME "Clash-Flux"
    BUNDLE_IDENTIFIER "dev.farna.clashflux"
    ${CLASHFLUX_APP_RESOURCE_OPTIONS})

huxerui_use_library(clash-flux TARGET HuxerUI::Camera
    URL "${CLASHFLUX_HUXERUI_CAMERA_URL}"
    COMMIT "${CLASHFLUX_HUXERUI_CAMERA_COMMIT}")
huxerui_use_library(clash-flux TARGET HuxerUI::SQLite
    URL "${CLASHFLUX_HUXERUI_SQLITE_URL}"
    COMMIT "${CLASHFLUX_HUXERUI_SQLITE_COMMIT}")
huxerui_use_library(clash-flux TARGET HuxerUI::Charts
    URL "${CLASHFLUX_HUXERUI_CHARTS_URL}"
    COMMIT "${CLASHFLUX_HUXERUI_CHARTS_COMMIT}")
if(ANDROID OR HUXERUI_LIBRARY_GRAPH_ONLY)
    huxerui_use_library(clash-flux TARGET HuxerUI::Media
        URL "${CLASHFLUX_HUXERUI_MEDIA_URL}"
        COMMIT "${CLASHFLUX_HUXERUI_MEDIA_COMMIT}")
endif()
set(_clashflux_previous_huxerui_home "${HUXERUI_HOME}")
if(CLASHFLUX_HUXERUI_SOURCE_DIR)
    set(HUXERUI_HOME "${CLASHFLUX_HUXERUI_SOURCE_DIR}")
else()
    set(HUXERUI_HOME "${HUXERUI_SDK_DIR}")
endif()
huxerui_use_library(clash-flux TARGET HuxerUI::CodeEditor
    URL "${CLASHFLUX_HUXERUI_CODEEDITOR_URL}"
    COMMIT "${CLASHFLUX_HUXERUI_CODEEDITOR_COMMIT}")
set(HUXERUI_HOME "${_clashflux_previous_huxerui_home}")

if(NOT HUXERUI_LIBRARY_GRAPH_ONLY)
    if(ANDROID)
        set_target_properties(clash-flux PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    else()
        target_sources(clash-flux PRIVATE FILE_SET clashflux_modules TYPE CXX_MODULES
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CLASHFLUX_MODULE_UNITS})
        set_target_properties(clash-flux PROPERTIES CXX_MODULE_STD ON CXX_SCAN_FOR_MODULES ON)
    endif()
    target_compile_features(clash-flux PRIVATE cxx_std_23)
    target_compile_definitions(clash-flux PRIVATE CLASHFLUX_VERSION="${PROJECT_VERSION}")
    if(IOS)
        target_compile_definitions(clash-flux PRIVATE CLASHFLUX_IOS=1)
    endif()
    target_include_directories(clash-flux PRIVATE src src/ui)
    if(ANDROID)
        target_include_directories(clash-flux PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/android-legacy" platform/android)
    endif()
    target_link_libraries(clash-flux PRIVATE
        clashflux_json clashflux_wire
        QrCode::qrcodegen yaml-cpp::yaml-cpp zstd::libzstd_static Threads::Threads)
    if(ZLIB_FOUND)
        target_compile_definitions(clash-flux PRIVATE CLASHFLUX_HAVE_ZLIB=1)
        target_link_libraries(clash-flux PRIVATE ZLIB::ZLIB)
    endif()
    if(ANDROID)
        target_link_libraries(clash-flux PRIVATE log)
    endif()
    target_compile_definitions(clash-flux PRIVATE CLASHFLUX_SINGBOX_VERSION="1.14.4")
endif()
