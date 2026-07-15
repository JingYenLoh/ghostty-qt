if(NOT DEFINED ZIG_EXECUTABLE OR NOT DEFINED GHOSTTY_SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "GenerateTerminfo.cmake requires ZIG_EXECUTABLE, GHOSTTY_SOURCE_DIR, and OUTPUT_FILE")
endif()

get_filename_component(OUTPUT_DIRECTORY "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")

execute_process(
    COMMAND "${ZIG_EXECUTABLE}" run
        --cache-dir ${OUTPUT_DIRECTORY}/zig-cache
        --global-cache-dir ${OUTPUT_DIRECTORY}/zig-global-cache
        --dep ghostty_terminfo
        -Mroot=${CMAKE_CURRENT_LIST_DIR}/GenerateTerminfo.zig
        -Mghostty_terminfo=${GHOSTTY_SOURCE_DIR}/src/terminfo/ghostty.zig
    WORKING_DIRECTORY "${GHOSTTY_SOURCE_DIR}"
    OUTPUT_FILE "${OUTPUT_FILE}"
    ERROR_VARIABLE GENERATE_TERMINFO_ERROR
    RESULT_VARIABLE GENERATE_TERMINFO_RESULT)

if(NOT GENERATE_TERMINFO_RESULT EQUAL 0)
    file(REMOVE "${OUTPUT_FILE}")
    message(FATAL_ERROR "Ghostty terminfo generation failed: ${GENERATE_TERMINFO_ERROR}")
endif()
