foreach(required_variable
        ZIG_EXECUTABLE SOURCE_DIR URL_SOURCE CACHE_DIR GLOBAL_CACHE_DIR PREFIX
        MATCHER_LIBRARY ONIGURUMA_LIBRARY LOCK_FILE OPTIMIZE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

# Developer, release, and sanitizer trees can share each revision/mode output.
# Serialize the install transaction because Zig's prefix is not transactional.
file(LOCK "${LOCK_FILE}" GUARD PROCESS TIMEOUT 1800 RESULT_VARIABLE lock_result)
if(lock_result)
    message(FATAL_ERROR "Unable to lock Ghostty link matcher build: ${lock_result}")
endif()

execute_process(
    COMMAND "${ZIG_EXECUTABLE}" build -j2
        --cache-dir "${CACHE_DIR}"
        --global-cache-dir "${GLOBAL_CACHE_DIR}"
        "-Dghostty-url-path=${URL_SOURCE}"
        "-Doptimize=${OPTIMIZE}"
        --prefix "${PREFIX}"
        --summary failures
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE zig_result)
if(NOT zig_result EQUAL 0)
    message(FATAL_ERROR "Pinned Ghostty link matcher build failed (${zig_result})")
endif()

foreach(output_file "${MATCHER_LIBRARY}" "${ONIGURUMA_LIBRARY}")
    if(NOT EXISTS "${output_file}")
        message(FATAL_ERROR "Ghostty link matcher did not produce ${output_file}")
    endif()
endforeach()
