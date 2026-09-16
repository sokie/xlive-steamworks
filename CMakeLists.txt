cmake_minimum_required(VERSION 3.18 FATAL_ERROR)

cmake_policy(SET CMP0091 NEW) # CMAKE_MSVC_RUNTIME_LIBRARY

project(
    xlive-steamworks
    VERSION 0.1.0
    DESCRIPTION "Games for Windows LIVE (xlive.dll) API on top of the Steamworks SDK"
    LANGUAGES C CXX
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Use a separate build directory.")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# The dll ships next to a title's exe, so it must not depend on a Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Steamworks SDK -------------------------------------------------------------------------------
# Point STEAMWORKS_SDK_DIR at an unpacked SDK (the directory that holds public/ and
# redistributable_bin/). Without it the rlabrecque mirror is fetched at the pinned revision.
set(STEAMWORKS_SDK_DIR "" CACHE PATH "Path to the Steamworks SDK root (contains public/steam)")
# v1.62, the newest SDK Proton 9 bridges, so a 1.62 build runs on every Proton and on Windows, where
# the client ships the matching interfaces. v1.65 is df2baabf574a738ef1ea90a7e89339107fc0a279.
set(STEAMWORKS_SDK_GIT_TAG "34d9338aa892213861bd9f84c4dd9c5b7fd23de7" CACHE STRING "Commit of the rlabrecque mirror to fetch when STEAMWORKS_SDK_DIR is not set")

if(NOT STEAMWORKS_SDK_DIR)
    include(cmake/CPM.cmake)
    # No GIT_SHALLOW: the pin is a commit that is not the default branch tip, and a shallow clone
    # fetches only the tip, so the checkout of the pinned commit would fail.
    CPMAddPackage(
        NAME SteamworksSDK
        GITHUB_REPOSITORY rlabrecque/SteamworksSDK
        GIT_TAG ${STEAMWORKS_SDK_GIT_TAG}
        DOWNLOAD_ONLY TRUE
    )
    set(STEAMWORKS_SDK_DIR "${SteamworksSDK_SOURCE_DIR}")
endif()

if(NOT EXISTS "${STEAMWORKS_SDK_DIR}/public/steam/steam_api.h")
    message(FATAL_ERROR "Steamworks SDK not found at '${STEAMWORKS_SDK_DIR}' (expected public/steam/steam_api.h).")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(STEAM_API_LIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.lib")
    set(STEAM_API_DLL "${STEAMWORKS_SDK_DIR}/redistributable_bin/win64/steam_api64.dll")
else()
    set(STEAM_API_LIB "${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.lib")
    set(STEAM_API_DLL "${STEAMWORKS_SDK_DIR}/redistributable_bin/steam_api.dll")
endif()

# SDK 1.63 replaced IsSteamRunningOnSteamDeck with IsRunningOnSteamHardware, 1.62 still builds.
file(STRINGS "${STEAMWORKS_SDK_DIR}/public/steam/isteamutils.h" xls_steam_hardware REGEX "IsRunningOnSteamHardware")
if(xls_steam_hardware)
    set(XLS_STEAM_HARDWARE_API 1)
else()
    set(XLS_STEAM_HARDWARE_API 0)
endif()

add_library(steam_api SHARED IMPORTED)
set_target_properties(steam_api PROPERTIES
    IMPORTED_IMPLIB "${STEAM_API_LIB}"
    IMPORTED_LOCATION "${STEAM_API_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${STEAMWORKS_SDK_DIR}/public"
)

option(XLS_BUILD_STATIC "Build xlive-steamworks as a static library to link into a title" OFF)

# --- xlive.dll ------------------------------------------------------------------------------------
file(GLOB_RECURSE xls_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
file(GLOB_RECURSE xls_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
set(xls_def "${CMAKE_CURRENT_SOURCE_DIR}/src/xlive_exports.def")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${xls_headers} ${xls_sources} ${xls_def})

if(XLS_BUILD_STATIC)
    add_library(xlive STATIC ${xls_headers} ${xls_sources})
else()
    add_library(xlive SHARED ${xls_headers} ${xls_sources} ${xls_def})
endif()

# A title that links the static library includes <xlive/xfuncs.h> and <xlive/xlive_steamworks.h>.
target_include_directories(xlive PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(xlive PUBLIC steam_api PRIVATE ws2_32 iphlpapi winmm comctl32 shlwapi)

set_target_properties(xlive PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "xlive"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>"
)

target_compile_definitions(xlive PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _WINSOCK_DEPRECATED_NO_WARNINGS
    "XLS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
    "XLS_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
    "XLS_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
    "XLS_STEAM_HARDWARE_API=${XLS_STEAM_HARDWARE_API}"
)

target_compile_options(xlive PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<CXX_COMPILER_ID:MSVC>:/wd4100> # unreferenced formal parameter: the GFWL surface has many.
    $<$<CXX_COMPILER_ID:MSVC>:/wd4201> # nameless struct/union in the SDK types.
    $<$<CXX_COMPILER_ID:MSVC>:/Zi>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/O2>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/Oi>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/Od>
)

target_link_options(xlive PRIVATE
    /SUBSYSTEM:WINDOWS
    /DEBUG
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>
    $<$<CONFIG:Debug>:/SAFESEH:NO>
)

# A title loads steam_api.dll from its own directory, so ship the two together.
if(NOT XLS_BUILD_STATIC)
add_custom_command(TARGET xlive POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${STEAM_API_DLL}" "$<TARGET_FILE_DIR:xlive>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/xlive_steamworks.example.json" "$<TARGET_FILE_DIR:xlive>/xlive_steamworks.example.json"
    COMMENT "Copying steam_api.dll and the example configuration next to xlive.dll"
)
endif()

# Vendored code keeps its own style, its narrowing warnings are not ours to fix.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/third-party/puff.c" PROPERTIES COMPILE_OPTIONS "/wd4244")

option(XLS_BUILD_TESTS "Build the smoke test that drives xlive.dll against a running Steam client" OFF)
if(XLS_BUILD_TESTS AND NOT XLS_BUILD_STATIC)
    add_executable(xlive_smoke tests/smoke/smoke.cpp tests/smoke/pair.cpp)
    target_link_libraries(xlive_smoke PRIVATE xlive ws2_32)
    set_target_properties(xlive_smoke PROPERTIES
        CXX_STANDARD 17
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/$<0:>"
    )
    target_compile_definitions(xlive_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS "XLS_STEAM_HARDWARE_API=${XLS_STEAM_HARDWARE_API}")
    target_link_options(xlive_smoke PRIVATE /SUBSYSTEM:CONSOLE)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT xlive)
