include_guard(GLOBAL)

# Configure the pinned libghostty-vt Zig build and its build contract.

# Keep the Zig-built engine identical between ordinary Debug and Release C++
# builds. Ghostty writes every configuration into its source-tree zig-out
# directory, so presets must not be built concurrently in one checkout.
set(_GHOSTTY_QT_DEFAULT_ZIG_FLAGS "-Doptimize=fast")
set(GHOSTTY_ZIG_BUILD_FLAGS
    "${_GHOSTTY_QT_DEFAULT_ZIG_FLAGS}"
    CACHE STRING
    "Additional flags passed to Ghostty's Zig build"
)

# Accept existing local build caches from the previous pinned Zig toolchain.
foreach(
    _mode
    IN
    ITEMS
        "ReleaseFast=fast"
        "ReleaseSafe=safe"
        "ReleaseSmall=small"
        "Debug=debug"
)
    string(REPLACE "=" ";" _pair "${_mode}")
    list(GET _pair 0 _old)
    list(GET _pair 1 _new)
    string(
        REPLACE "-Doptimize=${_old}"
        "-Doptimize=${_new}"
        GHOSTTY_ZIG_BUILD_FLAGS
        "${GHOSTTY_ZIG_BUILD_FLAGS}"
    )
endforeach()
if(NOT GHOSTTY_ZIG_BUILD_FLAGS MATCHES "-Doptimize=")
    list(APPEND GHOSTTY_ZIG_BUILD_FLAGS "-Doptimize=fast")
endif()

# The upstream CMake wrapper still appends the pre-0.17 ReleaseFast spelling
# for release configurations. Supply optimization explicitly and suppress that
# mapping while adding its targets below; keep the official checkout pristine.
set(GHOSTTY_QT_EFFECTIVE_LIB_VT_ZIG_FLAGS ${GHOSTTY_ZIG_BUILD_FLAGS})
string(
    JOIN " "
    GHOSTTY_QT_EFFECTIVE_LIB_VT_ZIG_FLAGS_TEXT
    ${GHOSTTY_QT_EFFECTIVE_LIB_VT_ZIG_FLAGS}
)

find_program(GHOSTTY_QT_ZIG_EXECUTABLE zig)
if(NOT GHOSTTY_QT_ZIG_EXECUTABLE)
    message(
        FATAL_ERROR
        "Zig 0.17.0 was not found on PATH. Install that exact toolchain "
        "before configuring ghostty-qt."
    )
endif()
execute_process(
    COMMAND "${GHOSTTY_QT_ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE GHOSTTY_QT_ZIG_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GHOSTTY_QT_ZIG_VERSION_RESULT
)
if(
    NOT GHOSTTY_QT_ZIG_VERSION_RESULT EQUAL 0
    OR NOT GHOSTTY_QT_ZIG_VERSION STREQUAL "0.17.0"
)
    message(
        FATAL_ERROR
        "The pinned Ghostty revision requires Zig 0.17.0; found "
        "'${GHOSTTY_QT_ZIG_VERSION}' at ${GHOSTTY_QT_ZIG_EXECUTABLE}."
    )
endif()

set(GHOSTTY_QT_LIB_VT_SOURCE_REVISION "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}")
if(
    DEFINED GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
    AND DEFINED GHOSTTY_QT_GIT_RESULT
    AND GHOSTTY_QT_GIT_RESULT EQUAL 0
)
    set(GHOSTTY_QT_LIB_VT_SOURCE_REVISION
        "${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}"
    )
endif()
string(
    CONCAT GHOSTTY_QT_LIB_VT_BUILD_CONTRACT_TEXT
    "revision=${GHOSTTY_QT_LIB_VT_SOURCE_REVISION}\n"
    "zig=${GHOSTTY_QT_ZIG_VERSION}\n"
    "system=${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}\n"
    "flags=${GHOSTTY_QT_EFFECTIVE_LIB_VT_ZIG_FLAGS_TEXT}"
)
string(
    SHA256 GHOSTTY_QT_LIB_VT_BUILD_CONTRACT
    "${GHOSTTY_QT_LIB_VT_BUILD_CONTRACT_TEXT}"
)

# Ghostty's wrapper writes libghostty-vt into the submodule's source-tree
# zig-out directory, but its custom command has no source dependencies. Guard
# that shared output with the complete native build contract so a submodule,
# toolchain, architecture, or optimization change cannot silently retain
# headers and libraries from the previous ABI. The prepared marker proves that
# a configure-time cleanup preceded the next upstream build; the build-time
# verifier below still repairs switches between already-configured presets.
set(GHOSTTY_QT_LIB_VT_ZIG_OUT_DIR "${GHOSTTY_SOURCE_DIR}/zig-out")
set(GHOSTTY_QT_LIB_VT_CONTRACT_STAMP
    "${GHOSTTY_QT_LIB_VT_ZIG_OUT_DIR}/.ghostty-qt-build-contract"
)
set(GHOSTTY_QT_LIB_VT_PREPARED_STAMP
    "${GHOSTTY_QT_LIB_VT_ZIG_OUT_DIR}/.ghostty-qt-prepared-${GHOSTTY_QT_LIB_VT_BUILD_CONTRACT}"
)
set(GHOSTTY_QT_BUILT_LIB_VT_CONTRACT "")
if(EXISTS "${GHOSTTY_QT_LIB_VT_CONTRACT_STAMP}")
    file(
        READ "${GHOSTTY_QT_LIB_VT_CONTRACT_STAMP}"
        GHOSTTY_QT_BUILT_LIB_VT_CONTRACT
    )
    string(
        STRIP "${GHOSTTY_QT_BUILT_LIB_VT_CONTRACT}"
        GHOSTTY_QT_BUILT_LIB_VT_CONTRACT
    )
endif()
if(
    NOT GHOSTTY_QT_BUILT_LIB_VT_CONTRACT STREQUAL GHOSTTY_QT_LIB_VT_BUILD_CONTRACT
)
    file(REMOVE_RECURSE "${GHOSTTY_QT_LIB_VT_ZIG_OUT_DIR}")
    file(MAKE_DIRECTORY "${GHOSTTY_QT_LIB_VT_ZIG_OUT_DIR}")
    file(
        WRITE "${GHOSTTY_QT_LIB_VT_PREPARED_STAMP}"
        "${GHOSTTY_QT_LIB_VT_BUILD_CONTRACT}\n"
    )
endif()

# Ghostty's wrapper invokes Zig and exports the ghostty-vt-static target.
set(_GHOSTTY_QT_SAVED_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
set(CMAKE_BUILD_TYPE "")
add_subdirectory(
    "${GHOSTTY_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/ghostty"
    EXCLUDE_FROM_ALL
)
set(CMAKE_BUILD_TYPE "${_GHOSTTY_QT_SAVED_BUILD_TYPE}")
unset(_GHOSTTY_QT_SAVED_BUILD_TYPE)

get_target_property(
    GHOSTTY_QT_LIB_VT_SHARED_LIBRARY
    ghostty-vt
    IMPORTED_LOCATION
)
get_target_property(
    GHOSTTY_QT_LIB_VT_STATIC_LIBRARY
    ghostty-vt-static
    IMPORTED_LOCATION
)
get_target_property(
    GHOSTTY_QT_LIB_VT_INCLUDE_DIRS
    ghostty-vt-static
    INTERFACE_INCLUDE_DIRECTORIES
)
list(GET GHOSTTY_QT_LIB_VT_INCLUDE_DIRS 0 GHOSTTY_QT_LIB_VT_INCLUDE_DIR)
set(GHOSTTY_QT_LIB_VT_HEADER
    "${GHOSTTY_QT_LIB_VT_INCLUDE_DIR}/ghostty/vt/terminal.h"
)
set(GHOSTTY_QT_LIB_VT_CONTRACT_LOCK
    "${CMAKE_CURRENT_SOURCE_DIR}/.cache/ghostty-lib-vt/build.lock"
)
set(GHOSTTY_QT_LIB_VT_ENSURE_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureGhosttyLibVtContract.cmake"
)
set(GHOSTTY_QT_LIB_VT_WRITE_STAMP_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteBuildContractStamp.cmake"
)

# This verifier deliberately runs on every consuming build. The upstream
# target first gets an opportunity to produce missing outputs; afterward the
# verifier compares stamp contents and performs the same Zig build itself when
# another configured preset left otherwise up-to-date files for a different
# contract. Consumers wait for the authoritative stamp to be written.
add_custom_target(
    ghostty-qt-lib-vt-contract-stamp
    COMMAND
        "${CMAKE_COMMAND}" "-DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}"
        "-DSOURCE_DIR=${GHOSTTY_SOURCE_DIR}"
        "-DZIG_OUT_DIR=${GHOSTTY_QT_LIB_VT_ZIG_OUT_DIR}"
        "-DZIG_FLAGS=${GHOSTTY_QT_EFFECTIVE_LIB_VT_ZIG_FLAGS}"
        "-DEXPECTED_CONTRACT=${GHOSTTY_QT_LIB_VT_BUILD_CONTRACT}"
        "-DCONTRACT_STAMP=${GHOSTTY_QT_LIB_VT_CONTRACT_STAMP}"
        "-DPREPARED_STAMP=${GHOSTTY_QT_LIB_VT_PREPARED_STAMP}"
        "-DSHARED_LIBRARY=${GHOSTTY_QT_LIB_VT_SHARED_LIBRARY}"
        "-DSTATIC_LIBRARY=${GHOSTTY_QT_LIB_VT_STATIC_LIBRARY}"
        "-DHEADER=${GHOSTTY_QT_LIB_VT_HEADER}"
        "-DLOCK_FILE=${GHOSTTY_QT_LIB_VT_CONTRACT_LOCK}"
        "-DWRITE_STAMP_SCRIPT=${GHOSTTY_QT_LIB_VT_WRITE_STAMP_SCRIPT}" -P
        "${GHOSTTY_QT_LIB_VT_ENSURE_SCRIPT}"
    DEPENDS
        "${GHOSTTY_QT_LIB_VT_ENSURE_SCRIPT}"
        "${GHOSTTY_QT_LIB_VT_WRITE_STAMP_SCRIPT}"
    COMMENT "Ensuring pinned libghostty-vt build contract"
    VERBATIM
)
add_dependencies(ghostty-qt-lib-vt-contract-stamp zig_build_lib_vt)
add_dependencies(ghostty-vt ghostty-qt-lib-vt-contract-stamp)
add_dependencies(ghostty-vt-static ghostty-qt-lib-vt-contract-stamp)

set(GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/.cache/zig-global"
    CACHE PATH
    "Project-local Zig package and artifact cache"
)
