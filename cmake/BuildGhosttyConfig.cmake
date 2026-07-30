foreach(required_variable
        ZIG_EXECUTABLE SOURCE_DIR CACHE_DIR GLOBAL_CACHE_DIR PREFIX
        LIBRARY RUNTIME_LIBRARY LOCK_FILE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

# Developer, release, and sanitizer trees share one revision-scoped Zig cache
# and prefix. Serialize the complete install/copy transaction so two Ninja
# processes cannot mutate those outputs concurrently.
file(LOCK "${LOCK_FILE}" GUARD PROCESS TIMEOUT 1800 RESULT_VARIABLE lock_result)
if(lock_result)
    message(FATAL_ERROR "Unable to lock Ghostty config parser build: ${lock_result}")
endif()

execute_process(
    COMMAND "${ZIG_EXECUTABLE}" build -j2
        --cache-dir "${CACHE_DIR}"
        --global-cache-dir "${GLOBAL_CACHE_DIR}"
        -Dapp-runtime=none
        -Demit-exe=false
        -Demit-lib-vt=false
        -Demit-docs=false
        -Demit-themes=false
        -Demit-terminfo=false
        -Demit-xcframework=false
        -Dsentry=false
        -Di18n=false
        -Dsimd=false
        -Dfont-backend=fontconfig_freetype
        -fsys=fontconfig
        -Doptimize=ReleaseFast
        --prefix "${PREFIX}"
        --summary failures
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE zig_result)
if(NOT zig_result EQUAL 0)
    message(FATAL_ERROR "Pinned Ghostty config parser build failed (${zig_result})")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${LIBRARY}" "${RUNTIME_LIBRARY}"
    RESULT_VARIABLE copy_result)
if(NOT copy_result EQUAL 0)
    message(FATAL_ERROR "Unable to stage Ghostty config runtime library (${copy_result})")
endif()
