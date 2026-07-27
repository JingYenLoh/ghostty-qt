foreach(_ghostty_qt_required_variable IN ITEMS
        SOURCE_DIR
        STAGE_DIR
        PATCH_EXECUTABLE
        PATCH_FILE
        VALIDATE_SCRIPT)
    if(NOT DEFINED ${_ghostty_qt_required_variable})
        message(FATAL_ERROR
            "${_ghostty_qt_required_variable} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE_DIR}")
file(MAKE_DIRECTORY "${STAGE_DIR}")
file(COPY "${SOURCE_DIR}/" DESTINATION "${STAGE_DIR}")

execute_process(
    COMMAND "${PATCH_EXECUTABLE}"
        --batch
        --forward
        --fuzz=0
        --no-backup-if-mismatch
        --strip=1
        "--input=${PATCH_FILE}"
    WORKING_DIRECTORY "${STAGE_DIR}"
    RESULT_VARIABLE _ghostty_qt_patch_result
    OUTPUT_VARIABLE _ghostty_qt_patch_output
    ERROR_VARIABLE _ghostty_qt_patch_error)
if(NOT _ghostty_qt_patch_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to apply the ghostty-qt shell-integration patch with zero fuzz.\n"
        "stdout:\n${_ghostty_qt_patch_output}\n"
        "stderr:\n${_ghostty_qt_patch_error}")
endif()

set(SHELL_INTEGRATION_DIR "${STAGE_DIR}")
include("${VALIDATE_SCRIPT}")
