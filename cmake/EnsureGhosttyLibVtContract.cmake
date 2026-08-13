foreach(
    required_variable
    ZIG_EXECUTABLE
    SOURCE_DIR
    ZIG_OUT_DIR
    EXPECTED_CONTRACT
    CONTRACT_STAMP
    PREPARED_STAMP
    SHARED_LIBRARY
    STATIC_LIBRARY
    HEADER
    LOCK_FILE
    WRITE_STAMP_SCRIPT
)
    if(
        NOT DEFINED ${required_variable}
        OR "${${required_variable}}" STREQUAL ""
    )
        message(FATAL_ERROR "Missing required variable ${required_variable}")
    endif()
endforeach()

string(LENGTH "${EXPECTED_CONTRACT}" expected_contract_length)
if(
    NOT EXPECTED_CONTRACT MATCHES "^[0-9a-f]+$"
    OR NOT expected_contract_length EQUAL 64
)
    message(FATAL_ERROR "Invalid expected libghostty-vt build contract")
endif()
if(NOT EXISTS "${WRITE_STAMP_SCRIPT}")
    message(FATAL_ERROR "Missing build-contract stamp writer")
endif()

cmake_path(GET LOCK_FILE PARENT_PATH lock_directory)
file(MAKE_DIRECTORY "${lock_directory}")
file(LOCK "${LOCK_FILE}" GUARD PROCESS TIMEOUT 1800 RESULT_VARIABLE lock_result)
if(lock_result)
    message(
        FATAL_ERROR
        "Unable to lock the libghostty-vt build contract: ${lock_result}"
    )
endif()

function(read_contract path output_variable)
    set(contract "")
    if(EXISTS "${path}")
        file(READ "${path}" contract)
        string(STRIP "${contract}" contract)
    endif()
    set(${output_variable} "${contract}" PARENT_SCOPE)
endfunction()

function(outputs_are_ready output_variable)
    set(ready TRUE)
    foreach(output IN ITEMS "${SHARED_LIBRARY}" "${STATIC_LIBRARY}" "${HEADER}")
        if(NOT EXISTS "${output}")
            set(ready FALSE)
        endif()
    endforeach()
    set(${output_variable} "${ready}" PARENT_SCOPE)
endfunction()

function(write_authoritative_stamp)
    set(OUTPUT "${CONTRACT_STAMP}")
    set(CONTRACT "${EXPECTED_CONTRACT}")
    include("${WRITE_STAMP_SCRIPT}")
endfunction()

read_contract("${CONTRACT_STAMP}" current_contract)
outputs_are_ready(outputs_ready)
if(current_contract STREQUAL EXPECTED_CONTRACT AND outputs_ready)
    return()
endif()

# A configure-time mismatch removes zig-out before writing this marker. If the
# upstream target has now produced all outputs, they necessarily belong to the
# expected contract and only the final authoritative stamp remains.
read_contract("${PREPARED_STAMP}" prepared_contract)
if(
    current_contract STREQUAL ""
    AND prepared_contract STREQUAL EXPECTED_CONTRACT
    AND outputs_ready
)
    write_authoritative_stamp()
    return()
endif()

# Existing outputs with a missing or different stamp may have been considered
# up to date by another build tree. Rebuild them in this invocation under the
# exact command contract used by Ghostty's CMake wrapper.
file(REMOVE_RECURSE "${ZIG_OUT_DIR}")
execute_process(
    COMMAND "${ZIG_EXECUTABLE}" build -Demit-lib-vt ${ZIG_FLAGS}
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE zig_result
)
if(NOT zig_result EQUAL 0)
    message(
        FATAL_ERROR
        "Unable to rebuild libghostty-vt for the expected contract (${zig_result})"
    )
endif()

outputs_are_ready(outputs_ready)
if(NOT outputs_ready)
    message(
        FATAL_ERROR
        "The libghostty-vt build completed without all required outputs"
    )
endif()
write_authoritative_stamp()
