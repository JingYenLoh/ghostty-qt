include_guard(GLOBAL)

# Build and expose the pinned Ghostty URL and path matcher.

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
    CACHE PATH
    "Project-local build area for Ghostty's URL/path matcher"
)
set(_GHOSTTY_QT_LINK_MATCHER_REVISION "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}")
if(
    DEFINED GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
    AND DEFINED GHOSTTY_QT_GIT_RESULT
    AND GHOSTTY_QT_GIT_RESULT EQUAL 0
)
    set(_GHOSTTY_QT_LINK_MATCHER_REVISION
        "${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}"
    )
endif()
set(GHOSTTY_QT_LINK_MATCHER_REVISION_DIR
    "${GHOSTTY_QT_LINK_MATCHER_BUILD_DIR}/${_GHOSTTY_QT_LINK_MATCHER_REVISION}/${_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE}"
)
set(GHOSTTY_QT_LINK_MATCHER_PREFIX
    "${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/prefix"
)
set(GHOSTTY_QT_LINK_MATCHER_CACHE_DIR
    "${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/cache"
)
set(GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR
    "${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/source"
)
set(GHOSTTY_QT_LINK_MATCHER_LIBRARY
    "${GHOSTTY_QT_LINK_MATCHER_PREFIX}/lib/libghostty-qt-link-matcher.a"
)
set(GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY
    "${GHOSTTY_QT_LINK_MATCHER_PREFIX}/lib/liboniguruma.a"
)
file(
    MAKE_DIRECTORY
        "${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
        "${GHOSTTY_QT_LINK_MATCHER_CACHE_DIR}"
        "${GHOSTTY_QT_LINK_MATCHER_PREFIX}/lib"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/oniguruma"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/apple-sdk"
)

# Build from a revision-scoped source shadow so an external Ghostty checkout
# supplies url.zig and its matching Oniguruma/Apple-SDK wrapper packages.
# Copying the small Zig packages also avoids a source-tree symlink that could race
# when multiple build trees select different intentional upgrade checkouts.
foreach(_ghostty_qt_matcher_source IN ITEMS build.zig build.zig.zon matcher.zig)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher/${_ghostty_qt_matcher_source}"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/${_ghostty_qt_matcher_source}"
        COPYONLY
    )
endforeach()
file(
    GLOB GHOSTTY_QT_LINK_MATCHER_APPLE_SDK_SOURCES
    CONFIGURE_DEPENDS
    "${GHOSTTY_SOURCE_DIR}/pkg/apple-sdk/*.zig"
    "${GHOSTTY_SOURCE_DIR}/pkg/apple-sdk/*.zig.zon"
)
foreach(
    _ghostty_qt_apple_sdk_source
    IN
    LISTS GHOSTTY_QT_LINK_MATCHER_APPLE_SDK_SOURCES
)
    cmake_path(
        GET _ghostty_qt_apple_sdk_source
        FILENAME _ghostty_qt_apple_sdk_name
    )
    configure_file(
        "${_ghostty_qt_apple_sdk_source}"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/apple-sdk/${_ghostty_qt_apple_sdk_name}"
        COPYONLY
    )
endforeach()
file(
    GLOB GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_SOURCES
    CONFIGURE_DEPENDS
    "${GHOSTTY_SOURCE_DIR}/pkg/oniguruma/*.zig"
    "${GHOSTTY_SOURCE_DIR}/pkg/oniguruma/*.zig.zon"
)
foreach(
    _ghostty_qt_oniguruma_source
    IN
    LISTS GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_SOURCES
)
    cmake_path(
        GET _ghostty_qt_oniguruma_source
        FILENAME _ghostty_qt_oniguruma_name
    )
    configure_file(
        "${_ghostty_qt_oniguruma_source}"
        "${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}/oniguruma/${_ghostty_qt_oniguruma_name}"
        COPYONLY
    )
endforeach()

add_custom_command(
    OUTPUT
        "${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        "${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}"
    COMMAND
        "${CMAKE_COMMAND}" "-DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}"
        "-DSOURCE_DIR=${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}"
        "-DURL_SOURCE=${GHOSTTY_SOURCE_DIR}/src/config/url.zig"
        "-DCACHE_DIR=${GHOSTTY_QT_LINK_MATCHER_CACHE_DIR}"
        "-DGLOBAL_CACHE_DIR=${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
        "-DPREFIX=${GHOSTTY_QT_LINK_MATCHER_PREFIX}"
        "-DMATCHER_LIBRARY=${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        "-DONIGURUMA_LIBRARY=${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}"
        "-DLOCK_FILE=${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/build.lock"
        "-DOPTIMIZE=${_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE}" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildGhosttyLinkMatcher.cmake"
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
    VERBATIM
)
add_custom_target(
    ghostty-qt-link-matcher-zig-build
    DEPENDS
        "${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        "${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}"
)

add_library(ghostty-qt-link-matcher-oniguruma STATIC IMPORTED GLOBAL)
set_target_properties(
    ghostty-qt-link-matcher-oniguruma
    PROPERTIES IMPORTED_LOCATION "${GHOSTTY_QT_LINK_MATCHER_ONIGURUMA_LIBRARY}"
)
add_dependencies(
    ghostty-qt-link-matcher-oniguruma
    ghostty-qt-link-matcher-zig-build
)

add_library(ghostty-qt-link-matcher-zig STATIC IMPORTED GLOBAL)
set_target_properties(
    ghostty-qt-link-matcher-zig
    PROPERTIES
        IMPORTED_LOCATION "${GHOSTTY_QT_LINK_MATCHER_LIBRARY}"
        INTERFACE_LINK_LIBRARIES ghostty-qt-link-matcher-oniguruma
)
add_dependencies(ghostty-qt-link-matcher-zig ghostty-qt-link-matcher-zig-build)
