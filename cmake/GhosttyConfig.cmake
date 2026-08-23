include_guard(GLOBAL)

# Build and expose Ghostty's private application configuration parser.

# Ghostty's exact application configuration parser is not part of
# libghostty-vt. Build the pinned internal library in an isolated cache and
# consume it only from a small helper process; no internal handle crosses into
# the Qt application. The source-local build area is shared by the checked-in
# CMake presets, with parser outputs isolated by Ghostty revision.
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    file(
        GLOB GHOSTTY_QT_FRAME_SOURCES
        CONFIGURE_DEPENDS
        "${GHOSTTY_SOURCE_DIR}/src/build/framegen/frames/*.txt"
    )
    pkg_check_modules(GHOSTTY_QT_FONTCONFIG REQUIRED fontconfig)
    set(GHOSTTY_QT_CONFIG_BUILD_PATCH
        "${CMAKE_CURRENT_SOURCE_DIR}/patches/ghostty-build-runtime-none-themes.patch"
    )
    set_property(
        DIRECTORY
        APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS "${GHOSTTY_QT_CONFIG_BUILD_PATCH}"
    )
    set(GHOSTTY_QT_CONFIG_BUILD_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ghostty-internal"
        CACHE PATH
        "Project-local build area for Ghostty's config parser"
    )
    set(GHOSTTY_QT_CONFIG_BUILD_REVISION
        "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}"
    )
    if(
        DEFINED GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
        AND DEFINED GHOSTTY_QT_GIT_RESULT
        AND GHOSTTY_QT_GIT_RESULT EQUAL 0
    )
        set(GHOSTTY_QT_CONFIG_BUILD_REVISION
            "${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}"
        )
    endif()
    set(GHOSTTY_QT_CONFIG_REVISION_DIR
        "${GHOSTTY_QT_CONFIG_BUILD_DIR}/${GHOSTTY_QT_CONFIG_BUILD_REVISION}"
    )
    set(GHOSTTY_QT_CONFIG_PREFIX "${GHOSTTY_QT_CONFIG_REVISION_DIR}/prefix")
    set(GHOSTTY_QT_CONFIG_CACHE_DIR "${GHOSTTY_QT_CONFIG_REVISION_DIR}/cache")
    # The private Qt integration needs one config export that isn't part of
    # Ghostty's public C API. Build the config library from a source shadow so
    # the pinned submodule remains pristine. Only src/ is copied; the larger
    # upstream dependency and resource directories are referenced by symlink.
    set(GHOSTTY_QT_CONFIG_SHADOW_DIR "${GHOSTTY_QT_CONFIG_REVISION_DIR}/source")
    set(GHOSTTY_QT_CONFIG_LIBRARY
        "${GHOSTTY_QT_CONFIG_PREFIX}/lib/ghostty-internal${CMAKE_SHARED_LIBRARY_SUFFIX}"
    )
    # The pinned build emits ghostty-internal.so but records libghostty.so as
    # its ELF SONAME. Keep a private copy under that runtime name so both the
    # build-tree and relocatable installation resolve DT_NEEDED correctly.
    set(GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY
        "${GHOSTTY_QT_CONFIG_PREFIX}/lib/libghostty${CMAKE_SHARED_LIBRARY_SUFFIX}"
    )
    set(GHOSTTY_QT_CONFIG_HEADER
        "${GHOSTTY_QT_CONFIG_PREFIX}/include/ghostty.h"
    )
    set(GHOSTTY_QT_CONFIG_THEMES_SOURCE_DIR
        "${GHOSTTY_QT_CONFIG_PREFIX}/share/ghostty/themes"
    )
    set(GHOSTTY_QT_THEMES_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/themes")
    set(GHOSTTY_QT_THEMES_STAGE_STAMP
        "${CMAKE_CURRENT_BINARY_DIR}/themes.stamp"
    )
    file(
        MAKE_DIRECTORY
            "${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
            "${GHOSTTY_QT_CONFIG_CACHE_DIR}"
            "${GHOSTTY_QT_CONFIG_PREFIX}/include"
    )

    # Multiple CMake build trees share this revision-scoped parser output.
    # Serialize the one-time shadow copy so a release reconfigure cannot
    # replace files under an active developer build (or vice versa).
    file(
        LOCK "${GHOSTTY_QT_CONFIG_REVISION_DIR}/source.lock"
        GUARD FILE
        TIMEOUT 120
    )
    set(GHOSTTY_QT_CONFIG_SHADOW_STAMP
        "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/.ghostty-qt-shadow-v3"
    )
    if(NOT EXISTS "${GHOSTTY_QT_CONFIG_SHADOW_STAMP}")
        file(REMOVE_RECURSE "${GHOSTTY_QT_CONFIG_SHADOW_DIR}")
        file(MAKE_DIRECTORY "${GHOSTTY_QT_CONFIG_SHADOW_DIR}")
        file(
            COPY "${GHOSTTY_SOURCE_DIR}/src"
            DESTINATION "${GHOSTTY_QT_CONFIG_SHADOW_DIR}"
        )
        configure_file(
            "${GHOSTTY_SOURCE_DIR}/build.zig.zon"
            "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/build.zig.zon"
            COPYONLY
        )
        foreach(
            _ghostty_qt_shadow_entry
            IN
            ITEMS
                dist
                example
                images
                include
                macos
                pkg
                po
                test
                vendor
                valgrind.supp
        )
            file(
                CREATE_LINK
                    "${GHOSTTY_SOURCE_DIR}/${_ghostty_qt_shadow_entry}"
                    "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/${_ghostty_qt_shadow_entry}"
                SYMBOLIC
                COPY_ON_ERROR
            )
        endforeach()
        configure_file(
            "${GHOSTTY_SOURCE_DIR}/src/config/CApi.zig"
            "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/src/config/CApi_upstream.zig"
            COPYONLY
        )

        # Retain the pinned checkout's version metadata when Git is available.
        # The generated gitfile points at metadata only; Git's configured
        # worktree remains the real, unmodified Ghostty submodule.
        if(Git_FOUND AND EXISTS "${GHOSTTY_SOURCE_DIR}/.git")
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --absolute-git-dir
                WORKING_DIRECTORY "${GHOSTTY_SOURCE_DIR}"
                OUTPUT_VARIABLE GHOSTTY_QT_GHOSTTY_GIT_DIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE GHOSTTY_QT_GHOSTTY_GIT_DIR_RESULT
            )
            if(GHOSTTY_QT_GHOSTTY_GIT_DIR_RESULT EQUAL 0)
                file(
                    WRITE "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/.git"
                    "gitdir: ${GHOSTTY_QT_GHOSTTY_GIT_DIR}\n"
                )
            endif()
        endif()
        file(WRITE "${GHOSTTY_QT_CONFIG_SHADOW_STAMP}" "ready\n")
    endif()
    configure_file(
        "${GHOSTTY_SOURCE_DIR}/build.zig"
        "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/build.zig"
        COPYONLY
    )
    configure_file(
        "${GHOSTTY_SOURCE_DIR}/src/config/Config.zig"
        "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/src/config/Config.zig"
        COPYONLY
    )
    execute_process(
        COMMAND
            "${GHOSTTY_QT_PATCH_EXECUTABLE}" --batch --forward --fuzz=0 -p1 -i
            "${GHOSTTY_QT_CONFIG_BUILD_PATCH}"
        WORKING_DIRECTORY "${GHOSTTY_QT_CONFIG_SHADOW_DIR}"
        RESULT_VARIABLE GHOSTTY_QT_CONFIG_BUILD_PATCH_RESULT
        OUTPUT_QUIET
    )
    if(NOT GHOSTTY_QT_CONFIG_BUILD_PATCH_RESULT EQUAL 0)
        message(
            FATAL_ERROR
            "Unable to patch the pinned Ghostty helper resource build"
        )
    endif()
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/zig/ghostty_config_capi_overlay.zig"
        "${GHOSTTY_QT_CONFIG_SHADOW_DIR}/src/config/CApi.zig"
        COPYONLY
    )

    add_custom_command(
        OUTPUT
            "${GHOSTTY_QT_CONFIG_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_HEADER}"
            "${GHOSTTY_QT_THEMES_STAGE_STAMP}"
        COMMAND
            "${CMAKE_COMMAND}" "-DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}"
            "-DSOURCE_DIR=${GHOSTTY_QT_CONFIG_SHADOW_DIR}"
            "-DCACHE_DIR=${GHOSTTY_QT_CONFIG_CACHE_DIR}"
            "-DGLOBAL_CACHE_DIR=${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
            "-DPREFIX=${GHOSTTY_QT_CONFIG_PREFIX}"
            "-DLIBRARY=${GHOSTTY_QT_CONFIG_LIBRARY}"
            "-DRUNTIME_LIBRARY=${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            "-DTHEMES_SOURCE_DIR=${GHOSTTY_QT_CONFIG_THEMES_SOURCE_DIR}"
            "-DTHEMES_STAGE_DIR=${GHOSTTY_QT_THEMES_STAGE_DIR}"
            "-DTHEMES_STAMP=${GHOSTTY_QT_THEMES_STAGE_STAMP}"
            "-DLOCK_FILE=${GHOSTTY_QT_CONFIG_REVISION_DIR}/build.lock" -P
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyConfig.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/zig/ghostty_config_capi_overlay.zig"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyConfig.cmake"
            "${GHOSTTY_QT_CONFIG_BUILD_PATCH}"
            "${GHOSTTY_SOURCE_DIR}/build.zig"
            "${GHOSTTY_SOURCE_DIR}/build.zig.zon"
            "${GHOSTTY_SOURCE_DIR}/include/ghostty.h"
            "${GHOSTTY_SOURCE_DIR}/src/cli/action.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/args.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/boo.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/crash_report.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/edit_config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ghostty.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/list_fonts.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/list_themes.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/lorem_ipsum.txt"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh_cache.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh-cache/DiskCache.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/ssh-cache/Entry.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/show_config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/show_face.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/tui.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/validate_config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/cli/version.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/CApi.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/Config.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/formatter.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/formatter_file.zig"
            "${GHOSTTY_SOURCE_DIR}/src/config/theme.zig"
            "${GHOSTTY_SOURCE_DIR}/src/crash/dir.zig"
            "${GHOSTTY_SOURCE_DIR}/src/crash/main.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/CodepointMap.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/CodepointResolver.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/Collection.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/DeferredFace.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/Metrics.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/SharedGrid.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/SharedGridSet.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/backend.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/discovery.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/library.zig"
            "${GHOSTTY_SOURCE_DIR}/src/font/main.zig"
            "${GHOSTTY_SOURCE_DIR}/src/input/Binding.zig"
            "${GHOSTTY_SOURCE_DIR}/src/build/GhosttyFrameData.zig"
            "${GHOSTTY_SOURCE_DIR}/src/build/framegen/main.c"
            ${GHOSTTY_QT_FRAME_SOURCES}
            "${GHOSTTY_SOURCE_DIR}/pkg/fontconfig/build.zig"
            "${GHOSTTY_SOURCE_DIR}/src/termio/shell_integration.zig"
            "${GHOSTTY_SOURCE_DIR}/src/terminal/color.zig"
            "${GHOSTTY_SOURCE_DIR}/src/terminfo/ghostty.zig"
        COMMENT "Building pinned Ghostty configuration parser"
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(
        ghostty-qt-config-parser
        DEPENDS
            "${GHOSTTY_QT_CONFIG_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            "${GHOSTTY_QT_CONFIG_HEADER}"
            "${GHOSTTY_QT_THEMES_STAGE_STAMP}"
    )
    add_library(ghostty-config-internal SHARED IMPORTED GLOBAL)
    set_target_properties(
        ghostty-config-internal
        PROPERTIES
            IMPORTED_LOCATION "${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GHOSTTY_QT_CONFIG_PREFIX}/include"
    )
    add_dependencies(ghostty-config-internal ghostty-qt-config-parser)
endif()
