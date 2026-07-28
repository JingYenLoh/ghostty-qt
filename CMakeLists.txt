cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0205)
    cmake_policy(SET CMP0205 NEW)
endif()

project(ghostty-qt VERSION 0.1.0 LANGUAGES C CXX)

set(GHOSTTY_QT_APPLICATION_ID "io.github.JingYenLoh.ghostty_qt")
set(GHOSTTY_QT_EFFECTIVE_APPLICATION_ID
    "${GHOSTTY_QT_APPLICATION_ID}$<$<CONFIG:Debug>:.Debug>")

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ghostty-qt supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

option(GHOSTTY_QT_BUILD_TESTS "Build ghostty-qt tests" ON)
option(GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY "Allow a Ghostty checkout other than the pinned revision" OFF)
option(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG
    "Build the pinned Ghostty application config parser" ON)

set(GHOSTTY_QT_GHOSTTY_REVISION_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/GHOSTTY_REVISION")
if(NOT EXISTS "${GHOSTTY_QT_GHOSTTY_REVISION_FILE}")
    message(FATAL_ERROR
        "Missing authoritative Ghostty pin: ${GHOSTTY_QT_GHOSTTY_REVISION_FILE}")
endif()
file(READ "${GHOSTTY_QT_GHOSTTY_REVISION_FILE}"
    GHOSTTY_QT_PINNED_GHOSTTY_REVISION)
string(STRIP "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}"
    GHOSTTY_QT_PINNED_GHOSTTY_REVISION)
string(LENGTH "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}"
    GHOSTTY_QT_PINNED_GHOSTTY_REVISION_LENGTH)
if(NOT GHOSTTY_QT_PINNED_GHOSTTY_REVISION MATCHES "^[0-9a-f]+$"
   OR NOT GHOSTTY_QT_PINNED_GHOSTTY_REVISION_LENGTH EQUAL 40)
    message(FATAL_ERROR
        "GHOSTTY_REVISION must contain one full lowercase Git commit hash")
endif()
set(GHOSTTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ghostty"
    CACHE PATH "Path to the pinned Ghostty checkout")

if(NOT EXISTS "${GHOSTTY_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Ghostty source not found at ${GHOSTTY_SOURCE_DIR}. "
        "Initialize the pinned ghostty submodule first.")
endif()

# Keep Ghostty's compatibility-heavy shell resources byte-for-byte except for
# the executable name used by their optional SSH wrappers. The downstream
# patch is deliberately applied to a build-tree copy with zero fuzz so a pin
# update cannot silently change the integration contract.
find_program(GHOSTTY_QT_PATCH_EXECUTABLE NAMES patch REQUIRED)
set(GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_DIR
    "${GHOSTTY_SOURCE_DIR}/src/shell-integration")
set(GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/shell-integration")
set(GHOSTTY_QT_SHELL_INTEGRATION_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/ghostty-shell-integration-ghostty-qt.patch")
set(GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateGhosttyShellIntegration.cmake")
set(GHOSTTY_QT_INSTALL_RESOURCES_DIR
    "${CMAKE_INSTALL_DATADIR}/ghostty-qt")
set(GHOSTTY_QT_INSTALL_SHELL_INTEGRATION_DIR
    "${GHOSTTY_QT_INSTALL_RESOURCES_DIR}/shell-integration")
file(GLOB_RECURSE GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_FILES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_DIR}/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GHOSTTY_QT_SHELL_INTEGRATION_PATCH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageGhosttyShellIntegration.cmake"
    "${GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT}"
    ${GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_FILES})
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_DIR}"
        "-DSTAGE_DIR=${GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR}"
        "-DPATCH_EXECUTABLE=${GHOSTTY_QT_PATCH_EXECUTABLE}"
        "-DPATCH_FILE=${GHOSTTY_QT_SHELL_INTEGRATION_PATCH}"
        "-DVALIDATE_SCRIPT=${GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageGhosttyShellIntegration.cmake"
    RESULT_VARIABLE GHOSTTY_QT_SHELL_INTEGRATION_STAGE_RESULT)
if(NOT GHOSTTY_QT_SHELL_INTEGRATION_STAGE_RESULT EQUAL 0)
    message(FATAL_ERROR
        "Unable to stage the pinned Ghostty shell-integration resources")
endif()

find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${GHOSTTY_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${GHOSTTY_SOURCE_DIR}"
        OUTPUT_VARIABLE GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GHOSTTY_QT_GIT_RESULT)
    if(GHOSTTY_QT_GIT_RESULT EQUAL 0
       AND NOT GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION STREQUAL GHOSTTY_QT_PINNED_GHOSTTY_REVISION
       AND NOT GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY)
        message(FATAL_ERROR
            "Ghostty is at ${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}, expected "
            "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}. Pass "
            "-DGHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY=ON only when intentionally testing an upgrade.")
    endif()
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS
    Core
    DBus
    Gui
    Multimedia
    Qml
    Quick
    QuickControls2
    ShaderTools
    Widgets
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XKBCommon REQUIRED IMPORTED_TARGET xkbcommon)
qt_standard_project_setup(REQUIRES 6.8)

# Keep the Zig-built engine identical between Debug and Release C++ builds.
# Ghostty writes both configurations into its source-tree zig-out directory,
# so developer and release builds must not be run concurrently.
set(_GHOSTTY_QT_DEFAULT_ZIG_FLAGS "")
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    set(_GHOSTTY_QT_DEFAULT_ZIG_FLAGS "-Doptimize=ReleaseFast")
endif()
set(GHOSTTY_ZIG_BUILD_FLAGS "${_GHOSTTY_QT_DEFAULT_ZIG_FLAGS}" CACHE STRING
    "Additional flags passed to Ghostty's Zig build")

find_program(GHOSTTY_QT_ZIG_EXECUTABLE zig)
if(NOT GHOSTTY_QT_ZIG_EXECUTABLE)
    message(FATAL_ERROR
        "Zig 0.15.2 was not found on PATH. Install that exact toolchain "
        "before configuring ghostty-qt.")
endif()
execute_process(
    COMMAND "${GHOSTTY_QT_ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE GHOSTTY_QT_ZIG_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GHOSTTY_QT_ZIG_VERSION_RESULT)
if(NOT GHOSTTY_QT_ZIG_VERSION_RESULT EQUAL 0 OR NOT GHOSTTY_QT_ZIG_VERSION STREQUAL "0.15.2")
    message(FATAL_ERROR
        "The pinned Ghostty revision requires Zig 0.15.2; found "
        "'${GHOSTTY_QT_ZIG_VERSION}' at ${GHOSTTY_QT_ZIG_EXECUTABLE}.")
endif()

# Ghostty's wrapper invokes Zig and exports the ghostty-vt-static target.
add_subdirectory("${GHOSTTY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ghostty" EXCLUDE_FROM_ALL)

set(GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.cache/zig-global"
    CACHE PATH "Project-local Zig package and artifact cache")

# Ghostty's exact application configuration parser is not part of
# libghostty-vt. Build the pinned internal library in an isolated cache and
# consume it only from a small helper process; no internal handle crosses into
# the Qt application. The source-local build area is shared by the checked-in
# CMake presets, with parser outputs isolated by Ghostty revision.
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    set(GHOSTTY_QT_CONFIG_BUILD_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ghostty-internal"
        CACHE PATH "Project-local build area for Ghostty's config parser")
    set(GHOSTTY_QT_CONFIG_BUILD_REVISION
        "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}")
    if(DEFINED GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
       AND DEFINED GHOSTTY_QT_GIT_RESULT
       AND GHOSTTY_QT_GIT_RESULT EQUAL 0)
        set(GHOSTTY_QT_CONFIG_BUILD_REVISION
            "${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}")
    endif()
    set(GHOSTTY_QT_CONFIG_REVISION_DIR
        "${GHOSTTY_QT_CONFIG_BUILD_DIR}/${GHOSTTY_QT_CONFIG_BUILD_REVISION}")
    set(GHOSTTY_QT_CONFIG_PREFIX
        "${GHOSTTY_QT_CONFIG_REVISION_DIR}/prefix")
    set(GHOSTTY_QT_CONFIG_CACHE_DIR
        "${GHOSTTY_QT_CONFIG_REVISION_DIR}/cache")
    # The private Qt integration needs one config export that isn't part of
    # Ghostty's public C API. Build the config library from a source shadow so
    # the pinned submodule remains pristine. Only src/ is copied; the larger
    # upstream dependency and resource directories are referenced by symlink.
    set(GHOSTTY_QT_CONFIG_SHADOW_DIR
        "${GHOSTTY_QT_CONFIG_REVISION_DIR}/source")
    set(GHOSTTY_QT_CONFIG_LIBRARY
        "${GHOSTTY_QT_CONFIG_PREFIX}/lib/ghostty-internal${CMAKE_SHARED_LIBRARY_SUFFIX}")
    # The pinned build emits ghostty-internal.so but records libghostty.so as
    # its ELF SONAME. Keep a private copy under that runtime name so both the
    # build-tree and relocatable installation resolve DT_NEEDED correctly.
    set(GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY
        "${GHOSTTY_QT_CONFIG_PREFIX}/lib/libghostty${CMAKE_SHARED_LIBRARY_SUFFIX}")
    set(GHOSTTY_QT_CONFIG_HEADER
        "${GHOSTTY_QT_CONFIG_PREFIX}/include/ghostty.h")
    file(MAKE_DIRECTORY
        "${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
        "${GHOSTTY_QT_CONFIG_CACHE_DIR}"
        "${GHOSTTY_QT_CONFIG_PREFIX}/include")

    # Multiple CMake build trees share this revision-scoped parser output.
    # Serialize the one-time shadow copy so a release reconfigure cannot
    # replace files under an active developer build (or vice versa).
    file(LOCK "${GHOSTTY_QT_CONFIG_REVISION_DIR}/source.lock"
        GUARD FILE TIMEOUT 120)
    set(GHOSTTY_QT_CONFIG_SHADOW_STAMP
        "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/.ghostty-qt-shadow-v1")
    if(NOT EXISTS "${GHOSTTY_QT_CONFIG_SHADOW_STAMP}")
        file(REMOVE_RECURSE "${GHOSTTY_QT_CONFIG_SHADOW_DIR}")
        file(MAKE_DIRECTORY "${GHOSTTY_QT_CONFIG_SHADOW_DIR}")
        file(COPY "${GHOSTTY_SOURCE_DIR}/src"
            DESTINATION "${GHOSTTY_QT_CONFIG_SHADOW_DIR}")
        configure_file("${GHOSTTY_SOURCE_DIR}/build.zig"
            "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/build.zig" COPYONLY)
        configure_file("${GHOSTTY_SOURCE_DIR}/build.zig.zon"
            "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/build.zig.zon" COPYONLY)
        foreach(_ghostty_qt_shadow_entry IN ITEMS
                dist example images include macos pkg po test vendor valgrind.supp)
            file(CREATE_LINK
                "${GHOSTTY_SOURCE_DIR}/${_ghostty_qt_shadow_entry}"
                "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/${_ghostty_qt_shadow_entry}"
                SYMBOLIC COPY_ON_ERROR)
        endforeach()
        configure_file("${GHOSTTY_SOURCE_DIR}/src/config/CApi.zig"
            "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/src/config/CApi_upstream.zig" COPYONLY)

        # Retain the pinned checkout's version metadata when Git is available.
        # The generated gitfile points at metadata only; Git's configured
        # worktree remains the real, unmodified Ghostty submodule.
        if(Git_FOUND AND EXISTS "${GHOSTTY_SOURCE_DIR}/.git")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
                WORKING_DIRECTORY "${GHOSTTY_SOURCE_DIR}"
                OUTPUT_VARIABLE GHOSTTY_QT_GHOSTTY_GIT_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE GHOSTTY_QT_GHOSTTY_GIT_DIR_RESULT)
            if(GHOSTTY_QT_GHOSTTY_GIT_DIR_RESULT EQUAL 0)
                file(WRITE "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/.git"
                    "gitdir: ${GHOSTTY_QT_GHOSTTY_GIT_DIR}\n")
            endif()
        endif()
        file(WRITE "${GHOSTTY_QT_CONFIG_SHADOW_STAMP}" "ready\n")
    endif()
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/zig/ghostty_config_capi_overlay.zig"
        "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/src/config/CApi.zig" COPYONLY)

    add_custom_command(
        OUTPUT
            "${GHOSTTY_QT_CONFIG_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_HEADER}"
        COMMAND "${CMAKE_COMMAND}"
            "-DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}"
            "-DSOURCE_DIR=${GHOSTTY_QT_CONFIG_SHADOW_DIR}"
            "-DCACHE_DIR=${GHOSTTY_QT_CONFIG_CACHE_DIR}"
            "-DGLOBAL_CACHE_DIR=${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
            "-DPREFIX=${GHOSTTY_QT_CONFIG_PREFIX}"
            "-DLIBRARY=${GHOSTTY_QT_CONFIG_LIBRARY}"
            "-DRUNTIME_LIBRARY=${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            "-DLOCK_FILE=${GHOSTTY_QT_CONFIG_REVISION_DIR}/build.lock"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyConfig.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/zig/ghostty_config_capi_overlay.zig"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyConfig.cmake"
            "${GHOSTTY_SOURCE_DIR}/build.zig"
            "${GHOSTTY_SOURCE_DIR}/build.zig.zon"
            "${GHOSTTY_SOURCE_DIR}/include/ghostty.h"
            "${GHOSTTY_SOURCE_DIR}/src/cli/action.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/args.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/edit_config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ghostty.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh_cache.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh-cache/DiskCache.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh-cache/Entry.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/show_config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/validate_config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/CApi.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/Config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/formatter.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/formatter_file.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/Metrics.zig"
            "${GHOSTTY_SOURCE_DIR}/src/input/Binding.zig"
            "${GHOSTTY_SOURCE_DIR}/src/termio/shell_integration.zig"
            "${GHOSTTY_SOURCE_DIR}/src/terminal/color.zig"
            "${GHOSTTY_SOURCE_DIR}/src/terminfo/ghostty.zig"
        COMMENT "Building pinned Ghostty configuration parser"
        USES_TERMINAL
        VERBATIM)
    add_custom_target(ghostty-qt-config-parser
        DEPENDS
            "${GHOSTTY_QT_CONFIG_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_HEADER}")
    add_dependencies(ghostty-qt-config-parser zig_build_lib_vt)

    add_library(ghostty-config-internal SHARED IMPORTED GLOBAL)
    set_target_properties(ghostty-config-internal PROPERTIES
        IMPORTED_LOCATION "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${GHOSTTY_QT_CONFIG_PREFIX}/include")
    add_dependencies(ghostty-config-internal ghostty-qt-config-parser)
endif()

# libghostty-vt deliberately omits Oniguruma. Keep Ghostty's default URL/path
# matcher in a narrow project-owned C ABI instead of broadening that upstream
# library. Both the expression and engine package come directly from the pinned
# checkout; no private terminal handle crosses this boundary.
set(_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE "ReleaseFast")
if(GHOSTTY_ZIG_BUILD_FLAGS MATCHES "-Doptimize=([A-Za-z]+)")
    set(_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE "${CMAKE_MATCH_1}")
endif()
set(GHOSTTY_QT_LINK_MATCHER_BUILD_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ghostty-link-matcher"
    CACHE PATH "Project-local build area for Ghostty's URL/path matcher")
set(_GHOSTTY_QT_LINK_MATCHER_REVISION
    "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}")
if(DEFINED GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
   AND DEFINED GHOSTTY_QT_GIT_RESULT
   AND GHOSTTY_QT_GIT_RESULT EQUAL 0)
    set(_GHOSTTY_QT_LINK_MATCHER_REVISION
        "${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}")
endif()
set(GHOSTTY_QT_LINK_MATCHER_REVISION_DIR
    "${GHOSTTY_QT_LINK_MATCHER_BUILD_DIR}/${_GHOSTTY_QT_LINK_MATCHER_REVISION}/${_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE}")
set(GHOSTTY_QT_LINK_MATCHER_PREFIX
    "${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/prefix")
set(GHOSTTY_QT_LINK_MATCHER_CACHE_DIR
    "${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/cache")
set(GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR
    "${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/source")
set(GHOSTTY_QT_LINK_MATCHER_LIBRARY
    "${GHOSTTY_QT_LINK_MATCHER_PREFIX}/lib/libghostty-qt-link-matcher.a")
set(GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY
    "${GHOSTTY_QT_LINK_MATCHER_PREFIX}/lib/liboniguruma.a")
file(MAKE_DIRECTORY
    "${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
    "${GHOSTTY_QT_LINK_MATCHER_CACHE_DIR}"
    "${GHOSTTY_QT_LINK_MATCHER_PREFIX}/lib"
    "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/oniguruma"
    "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/apple-sdk")

# Build from a revision-scoped source shadow so an external Ghostty checkout
# supplies url.zig and its matching Oniguruma/Apple-SDK wrapper packages.
# Copying the small Zig packages also avoids a source-tree symlink that could race
# when multiple build trees select different intentional upgrade checkouts.
foreach(_ghostty_qt_matcher_source IN ITEMS build.zig build.zig.zon matcher.zig)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher/${_ghostty_qt_matcher_source}"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/${_ghostty_qt_matcher_source}"
        COPYONLY)
endforeach()
file(GLOB GHOSTTY_QT_LINK_MATCHER_APPLE_SDK_SOURCES
    CONFIGURE_DEPENDS
    "${GHOSTTY_SOURCE_DIR}/pkg/apple-sdk/*.zig"
    "${GHOSTTY_SOURCE_DIR}/pkg/apple-sdk/*.zig.zon")
foreach(_ghostty_qt_apple_sdk_source IN LISTS
        GHOSTTY_QT_LINK_MATCHER_APPLE_SDK_SOURCES)
    cmake_path(GET _ghostty_qt_apple_sdk_source FILENAME
        _ghostty_qt_apple_sdk_name)
    configure_file(
        "${_ghostty_qt_apple_sdk_source}"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/apple-sdk/${_ghostty_qt_apple_sdk_name}"
        COPYONLY)
endforeach()
file(GLOB GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_SOURCES
    CONFIGURE_DEPENDS
    "${GHOSTTY_SOURCE_DIR}/pkg/oniguruma/*.zig"
    "${GHOSTTY_SOURCE_DIR}/pkg/oniguruma/*.zig.zon")
foreach(_ghostty_qt_oniguruma_source IN LISTS
        GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_SOURCES)
    cmake_path(GET _ghostty_qt_oniguruma_source FILENAME
        _ghostty_qt_oniguruma_name)
    configure_file(
        "${_ghostty_qt_oniguruma_source}"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/oniguruma/${_ghostty_qt_oniguruma_name}"
        COPYONLY)
endforeach()

add_custom_command(
    OUTPUT
        "${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        "${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}"
    COMMAND "${CMAKE_COMMAND}"
        "-DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}"
        "-DSOURCE_DIR=${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}"
        "-DURL_SOURCE=${GHOSTTY_SOURCE_DIR}/src/config/url.zig"
        "-DCACHE_DIR=${GHOSTTY_QT_LINK_MATCHER_CACHE_DIR}"
        "-DGLOBAL_CACHE_DIR=${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
        "-DPREFIX=${GHOSTTY_QT_LINK_MATCHER_PREFIX}"
        "-DMATCHER_LIBRARY=${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        "-DONIGURUMA_LIBRARY=${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}"
        "-DLOCK_FILE=${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/build.lock"
        "-DOPTIMIZE=${_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyLinkMatcher.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyLinkMatcher.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher/build.zig"
        "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher/build.zig.zon"
        "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher/matcher.zig"
        "${GHOSTTY_SOURCE_DIR}/src/config/url.zig"
        ${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_SOURCES}
        ${GHOSTTY_QT_LINK_MATCHER_APPLE_SDK_SOURCES}
    COMMENT "Building pinned Ghostty URL/path matcher"
    USES_TERMINAL
    VERBATIM)
add_custom_target(ghostty-qt-link-matcher-zig-build
    DEPENDS
        "${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        "${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}")

add_library(ghostty-qt-link-matcher-oniguruma STATIC IMPORTED GLOBAL)
set_target_properties(ghostty-qt-link-matcher-oniguruma PROPERTIES
    IMPORTED_LOCATION "${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}")
add_dependencies(ghostty-qt-link-matcher-oniguruma
    ghostty-qt-link-matcher-zig-build)

add_library(ghostty-qt-link-matcher-zig STATIC IMPORTED GLOBAL)
set_target_properties(ghostty-qt-link-matcher-zig PROPERTIES
    IMPORTED_LOCATION "${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
    INTERFACE_LINK_LIBRARIES ghostty-qt-link-matcher-oniguruma)
add_dependencies(ghostty-qt-link-matcher-zig
    ghostty-qt-link-matcher-zig-build)

add_library(ghostty-qt-link-matcher STATIC
    src/ghostty_link_matcher.cpp
    src/ghostty_link_matcher.h
)
target_include_directories(ghostty-qt-link-matcher
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher")
target_link_libraries(ghostty-qt-link-matcher
    PUBLIC Qt6::Core
    PRIVATE ghostty-qt-link-matcher-zig)
target_compile_options(ghostty-qt-link-matcher PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)

# libghostty-vt has public C struct fields named `emit`. Disabling Qt's
# convenience keywords avoids a preprocessor collision with that valid API.
add_compile_definitions(QT_NO_KEYWORDS)

find_program(GHOSTTY_QT_TIC_EXECUTABLE tic REQUIRED)
set(GHOSTTY_QT_TERMINFO_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/terminfo/ghostty.terminfo")
set(GHOSTTY_QT_TERMINFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/share/terminfo")
set(GHOSTTY_QT_TERMINFO_STAMP "${CMAKE_CURRENT_BINARY_DIR}/terminfo/compiled.stamp")
set(GHOSTTY_QT_INSTALL_TERMINFO_DIR
    "${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo")

# Keep the compiled resource locator relocatable. It records only the path from
# the installed executable directory to the private terminfo database, never a
# configure-time build or installation prefix.
set(_ghostty_qt_install_bindir "${CMAKE_INSTALL_BINDIR}")
cmake_path(ABSOLUTE_PATH _ghostty_qt_install_bindir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_bindir_absolute)
set(_ghostty_qt_install_resources_dir
    "${GHOSTTY_QT_INSTALL_RESOURCES_DIR}")
cmake_path(ABSOLUTE_PATH _ghostty_qt_install_resources_dir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_resources_dir_absolute)
cmake_path(RELATIVE_PATH _ghostty_qt_install_resources_dir_absolute
    BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
    OUTPUT_VARIABLE GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR)
set(_ghostty_qt_install_terminfo_dir "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")
cmake_path(ABSOLUTE_PATH _ghostty_qt_install_terminfo_dir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_terminfo_dir_absolute)
cmake_path(RELATIVE_PATH _ghostty_qt_install_terminfo_dir_absolute
    BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
    OUTPUT_VARIABLE GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR)
add_custom_command(
    OUTPUT "${GHOSTTY_QT_TERMINFO_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}
        -DGHOSTTY_SOURCE_DIR=${GHOSTTY_SOURCE_DIR}
        -DOUTPUT_FILE=${GHOSTTY_QT_TERMINFO_SOURCE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerminfo.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GHOSTTY_QT_TERMINFO_DIR}"
    COMMAND "${GHOSTTY_QT_TIC_EXECUTABLE}" -x -o "${GHOSTTY_QT_TERMINFO_DIR}"
        "${GHOSTTY_QT_TERMINFO_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${GHOSTTY_QT_TERMINFO_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerminfo.zig"
        "${GHOSTTY_SOURCE_DIR}/src/terminfo/Source.zig"
        "${GHOSTTY_SOURCE_DIR}/src/terminfo/ghostty.zig"
    COMMENT "Generating and compiling xterm-ghostty terminfo"
    VERBATIM)
add_custom_target(ghostty-qt-terminfo DEPENDS "${GHOSTTY_QT_TERMINFO_STAMP}")

add_library(ghostty-qt-runtime-paths STATIC
    src/terminfo_paths.cpp
    src/terminfo_paths.h
)
target_include_directories(ghostty-qt-runtime-paths PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-runtime-paths PUBLIC Qt6::Core)
target_compile_definitions(ghostty-qt-runtime-paths PRIVATE
    GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR}"
)

add_library(ghostty-qt-shell-integration STATIC
    src/ghostty_shell_integration.cpp
    src/ghostty_shell_integration.h
)
target_include_directories(ghostty-qt-shell-integration PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-shell-integration PUBLIC
    Qt6::Core
    Qt6::Gui
)
target_compile_definitions(ghostty-qt-shell-integration PRIVATE
    GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR}"
)
target_compile_options(ghostty-qt-shell-integration PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-cli-delegation STATIC
    src/ghostty_cli_delegation.cpp
    src/ghostty_cli_delegation.h
)
target_include_directories(ghostty-qt-cli-delegation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_options(ghostty-qt-cli-delegation PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-desktop-activation STATIC
    src/desktop_activation.cpp
    src/desktop_activation.h
)
target_include_directories(ghostty-qt-desktop-activation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-desktop-activation
    PUBLIC Qt6::Core
    PRIVATE Qt6::Gui
)
target_compile_options(ghostty-qt-desktop-activation PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-linux-cgroup STATIC
    src/linux_cgroup.cpp
    src/linux_cgroup.h
    src/linux_cgroup_config.h
)
target_include_directories(ghostty-qt-linux-cgroup PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-linux-cgroup PUBLIC
    Qt6::Core
    Qt6::DBus
)
target_compile_options(ghostty-qt-linux-cgroup PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-frontend-config STATIC
    src/frontend_config.cpp
    src/frontend_config.h
    src/frontend_config_service.cpp
    src/frontend_config_service.h
)
target_include_directories(ghostty-qt-frontend-config PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-frontend-config PUBLIC
    Qt6::Core
    Qt6::Gui
)
target_compile_options(ghostty-qt-frontend-config PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-action-pipeline STATIC
    src/ghostty_action_catalog.cpp
    src/ghostty_action_catalog.h
    src/ghostty_keybind_set.cpp
    src/ghostty_keybind_set.h
    src/zig_string_escape.cpp
    src/zig_string_escape.h
)
target_include_directories(ghostty-qt-action-pipeline PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-action-pipeline PUBLIC
    Qt6::Core
    Qt6::Gui
)
target_compile_options(ghostty-qt-action-pipeline PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-config-export STATIC
    src/ghostty_config_export.cpp
    src/ghostty_config_export.h
    src/ghostty_config_values.h
    src/ghostty_keybind_config.h
    src/terminal_command.h
)
target_include_directories(ghostty-qt-config-export PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-config-export PUBLIC
    Qt6::Core
    Qt6::Gui
)
target_compile_options(ghostty-qt-config-export PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-terminal-bell STATIC
    src/terminal_bell.cpp
    src/terminal_bell.h
)
target_include_directories(ghostty-qt-terminal-bell PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-terminal-bell
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        Qt6::Multimedia
        Qt6::Widgets
)
target_compile_options(ghostty-qt-terminal-bell PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(ghostty-qt-terminal-rendering STATIC
    src/terminal_backdrop.cpp
    src/terminal_backdrop.h
    src/terminal_backdrop_qsg.cpp
    src/terminal_backdrop_qsg.h
    src/terminal_rect_batch.cpp
    src/terminal_rect_batch.h
)
target_include_directories(ghostty-qt-terminal-rendering PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-terminal-rendering
    PUBLIC
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
)
target_compile_options(ghostty-qt-terminal-rendering PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
qt_add_shaders(
    ghostty-qt-terminal-rendering
    ghostty_qt_terminal_backdrop_shaders

    BATCHABLE

    PREFIX
        "/ghostty-qt/shaders"

    FILES
        shaders/terminal_backdrop.vert
        shaders/terminal_backdrop.frag

    OUTPUTS
        terminal_backdrop.vert.qsb
        terminal_backdrop.frag.qsb
)

add_library(ghostty-qt-vt-adapter STATIC
    src/ghostty_vt_adapter.cpp
    src/ghostty_vt_adapter.h
)
target_include_directories(ghostty-qt-vt-adapter PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-vt-adapter
    PUBLIC
        Qt6::Core
        Qt6::Gui
    PRIVATE
        ghostty-vt-static
)
target_compile_options(ghostty-qt-vt-adapter PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

qt_add_executable(ghostty-qt
    src/main.cpp
    src/application_action.h
    src/application_controller.cpp
    src/application_controller.h
    src/application_lifetime.cpp
    src/application_lifetime.h
    src/ghostty_application_keybindings.cpp
    src/ghostty_application_keybindings.h
    src/ghostty_config_loader.h
    src/ghostty_config_export.h
    src/ghostty_config_edit.cpp
    src/ghostty_config_edit.h
    src/ghostty_config_process_loader.cpp
    src/ghostty_config_process_loader.h
    src/ghostty_config_service.cpp
    src/ghostty_config_service.h
    src/ghostty_config_snapshot.h
    src/ghostty_config_values.h
    src/ghostty_keybind_config.h
    src/ghostty_global_shortcut_portal.cpp
    src/ghostty_global_shortcut_portal.h
    src/initial_session_coordinator.cpp
    src/initial_session_coordinator.h
    src/key_event_snapshot.h
    src/launch_options.cpp
    src/launch_options.h
    src/revision_counter.h
    src/session_worker.cpp
    src/session_worker.h
    src/single_instance_activation.cpp
    src/single_instance_activation.h
    src/tab_list_model.cpp
    src/tab_list_model.h
    src/terminal_action_result.h
    src/terminal_appearance.h
    src/terminal_controller.cpp
    src/terminal_controller.h
    src/terminal_clipboard.cpp
    src/terminal_clipboard.h
    src/terminal_cell_metrics.cpp
    src/terminal_cell_metrics.h
    src/terminal_command.h
    src/terminal_geometry.cpp
    src/terminal_geometry.h
    src/terminal_osc8_index.h
    src/terminal_pane.cpp
    src/terminal_pane.h
    src/terminal_session_options.h
    src/terminal_typography.h
    src/terminal_types.h
    src/terminal_write_file_action.h
    src/terminal_workspace.cpp
    src/terminal_workspace.h
    src/window_navigation_action.h
    src/workspace_action.h
    src/workspace_ids.h
)

qt_add_qml_module(ghostty-qt
    URI GhosttyQt
    VERSION 1.0
    QML_FILES
        qml/AbnormalExitOverlay.qml
        qml/BellBorderOverlay.qml
        qml/Main.qml
        qml/ReadOnlyOverlay.qml
        qml/ResizeOverlay.qml
        qml/SearchOverlay.qml
        qml/TerminalScrollBar.qml
)

target_link_libraries(ghostty-qt PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
    PkgConfig::XKBCommon
    ghostty-qt-action-pipeline
    ghostty-qt-cli-delegation
    ghostty-qt-config-export
    ghostty-qt-desktop-activation
    ghostty-qt-frontend-config
    ghostty-qt-linux-cgroup
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-shell-integration
    ghostty-qt-terminal-rendering
    ghostty-qt-terminal-bell
    ghostty-qt-vt-adapter
    util
)

target_compile_definitions(ghostty-qt PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
    GHOSTTY_QT_APPLICATION_ID="${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
    GHOSTTY_QT_CONFIG_ENABLED=$<BOOL:${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}>
)

add_dependencies(ghostty-qt ghostty-qt-terminfo)

target_compile_options(ghostty-qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    add_executable(ghostty-qt-config-helper
        src/ghostty_config_cli_main.cpp
    )
    target_link_libraries(ghostty-qt-config-helper PRIVATE
        ghostty-config-internal
        ghostty-qt-cli-delegation
    )
    target_compile_options(ghostty-qt-config-helper PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )

    set(_ghostty_qt_config_install_dir
        "${CMAKE_INSTALL_LIBDIR}/ghostty-qt")
    cmake_path(ABSOLUTE_PATH _ghostty_qt_config_install_dir
        BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
        NORMALIZE
        OUTPUT_VARIABLE _ghostty_qt_config_install_dir_absolute)
    cmake_path(RELATIVE_PATH _ghostty_qt_config_install_dir_absolute
        BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
        OUTPUT_VARIABLE _ghostty_qt_config_install_relative_dir)
    set_target_properties(ghostty-qt-config-helper PROPERTIES
        BUILD_RPATH "${GHOSTTY_QT_CONFIG_PREFIX}/lib"
        # Keep the build-tree RUNPATH exact rather than letting CMake append an
        # empty padding component (which the dynamic loader treats as cwd).
        # The install step rewrites this absolute path to the relative one.
        INSTALL_RPATH "${GHOSTTY_QT_CONFIG_PREFIX}/lib"
        BUILD_WITH_INSTALL_RPATH TRUE)

    target_compile_definitions(ghostty-qt PRIVATE
        GHOSTTY_QT_CONFIG_HELPER_NAME="ghostty-qt-config-helper")
    add_dependencies(ghostty-qt ghostty-qt-config-helper)

    if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}")
        set(_ghostty_qt_installed_helper
            "\$ENV{DESTDIR}${CMAKE_INSTALL_BINDIR}/ghostty-qt-config-helper")
    else()
        set(_ghostty_qt_installed_helper
            "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/ghostty-qt-config-helper")
    endif()
    # Force a fresh source binary on repeated installs before applying the
    # explicit RPATH rewrite below.
    install(CODE "file(REMOVE \"${_ghostty_qt_installed_helper}\")")
    install(TARGETS ghostty-qt-config-helper
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
        DESTINATION "${_ghostty_qt_config_install_dir}")
    string(CONCAT _ghostty_qt_install_rpath_code
        "file(RPATH_CHANGE\n"
        "  FILE \"${_ghostty_qt_installed_helper}\"\n"
        "  OLD_RPATH \"${GHOSTTY_QT_CONFIG_PREFIX}/lib\"\n"
        "  NEW_RPATH \"$ORIGIN/${_ghostty_qt_config_install_relative_dir}\")")
    install(CODE "${_ghostty_qt_install_rpath_code}")
endif()

set(_ghostty_qt_desktop_build_dir
    "${CMAKE_CURRENT_BINARY_DIR}/desktop-integration")
file(MAKE_DIRECTORY "${_ghostty_qt_desktop_build_dir}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/ghostty-qt.desktop.in"
    "${_ghostty_qt_desktop_build_dir}/ghostty-qt.desktop.in"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/linux/ghostty-qt.service.in"
    "${_ghostty_qt_desktop_build_dir}/ghostty-qt.service.in"
    @ONLY)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchInstalledDesktopMetadata.cmake.in"
    "${_ghostty_qt_desktop_build_dir}/PatchInstalledDesktopMetadata.cmake.in"
    @ONLY)
file(GENERATE
    OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.desktop"
    INPUT "${_ghostty_qt_desktop_build_dir}/ghostty-qt.desktop.in")
file(GENERATE
    OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.service"
    INPUT "${_ghostty_qt_desktop_build_dir}/ghostty-qt.service.in")
file(GENERATE
    OUTPUT
        "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/PatchInstalledDesktopMetadata.cmake"
    INPUT
        "${_ghostty_qt_desktop_build_dir}/PatchInstalledDesktopMetadata.cmake.in")

install(TARGETS ghostty-qt RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES
    "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES
    "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}.service"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/services")
install(SCRIPT
    "${_ghostty_qt_desktop_build_dir}/$<CONFIG>/PatchInstalledDesktopMetadata.cmake")
install(DIRECTORY "${GHOSTTY_QT_TERMINFO_DIR}/"
    DESTINATION "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")
install(DIRECTORY "${GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR}/"
    DESTINATION "${GHOSTTY_QT_INSTALL_SHELL_INTEGRATION_DIR}")

if(GHOSTTY_QT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
