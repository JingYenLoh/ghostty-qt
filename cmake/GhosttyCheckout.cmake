include_guard(GLOBAL)

# Validate and locate the pinned upstream Ghostty checkout.

set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${GHOSTTY_QT_GHOSTTY_REVISION_FILE}"
)
if(NOT EXISTS "${GHOSTTY_QT_GHOSTTY_REVISION_FILE}")
    message(
        FATAL_ERROR
        "Missing authoritative Ghostty pin: ${GHOSTTY_QT_GHOSTTY_REVISION_FILE}"
    )
endif()
file(
    READ "${GHOSTTY_QT_GHOSTTY_REVISION_FILE}"
    GHOSTTY_QT_PINNED_GHOSTTY_REVISION
)
string(
    STRIP "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}"
    GHOSTTY_QT_PINNED_GHOSTTY_REVISION
)
string(
    LENGTH "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}"
    GHOSTTY_QT_PINNED_GHOSTTY_REVISION_LENGTH
)
if(
    NOT GHOSTTY_QT_PINNED_GHOSTTY_REVISION MATCHES "^[0-9a-f]+$"
    OR NOT GHOSTTY_QT_PINNED_GHOSTTY_REVISION_LENGTH EQUAL 40
)
    message(
        FATAL_ERROR
        "GHOSTTY_REVISION must contain one full lowercase Git commit hash"
    )
endif()
set(GHOSTTY_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/ghostty"
    CACHE PATH
    "Path to the pinned Ghostty checkout"
)

if(NOT EXISTS "${GHOSTTY_SOURCE_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
        "Ghostty source not found at ${GHOSTTY_SOURCE_DIR}. "
        "Initialize the pinned ghostty submodule first."
    )
endif()

find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${GHOSTTY_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${GHOSTTY_SOURCE_DIR}"
        OUTPUT_VARIABLE GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GHOSTTY_QT_GIT_RESULT
    )
    if(
        GHOSTTY_QT_GIT_RESULT EQUAL 0
        AND NOT GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
            STREQUAL
            GHOSTTY_QT_PINNED_GHOSTTY_REVISION
        AND NOT GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY
    )
        message(
            FATAL_ERROR
            "Ghostty is at ${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}, expected "
            "${GHOSTTY_QT_PINNED_GHOSTTY_REVISION}. Pass "
            "-DGHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY=ON only when intentionally testing an upgrade."
        )
    endif()
endif()
