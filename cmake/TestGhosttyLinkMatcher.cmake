foreach(
    required_variable
    ZIG_EXECUTABLE
    SOURCE_DIR
    URL_SOURCE
    CACHE_DIR
    GLOBAL_CACHE_DIR
    LOCK_FILE
    OPTIMIZE
)
    if(
        NOT DEFINED ${required_variable}
        OR "${${required_variable}}" STREQUAL ""
    )
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

file(LOCK "${LOCK_FILE}" GUARD PROCESS TIMEOUT 1800 RESULT_VARIABLE lock_result)
if(lock_result)
    message(
        FATAL_ERROR
        "Unable to lock Ghostty link matcher tests: ${lock_result}"
    )
endif()

execute_process(
    COMMAND
        "${ZIG_EXECUTABLE}" build test -j2 --cache-dir "${CACHE_DIR}"
        --global-cache-dir "${GLOBAL_CACHE_DIR}"
        "-Dghostty-url-path=${URL_SOURCE}" "-Doptimize=${OPTIMIZE}" --summary
        failures
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE zig_result
)
if(NOT zig_result EQUAL 0)
    message(
        FATAL_ERROR
        "Pinned Ghostty URL/path matcher corpus failed (${zig_result})"
    )
endif()
