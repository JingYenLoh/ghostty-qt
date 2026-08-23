include_guard(GLOBAL)

# Stage Ghostty runtime resources and build the private terminfo database.

# Keep Ghostty's compatibility-heavy shell resources byte-for-byte except for
# the executable name used by their optional SSH wrappers. The downstream
# patch is deliberately applied to a build-tree copy with zero fuzz so a pin
# update cannot silently change the integration contract.
find_program(GHOSTTY_QT_PATCH_EXECUTABLE NAMES patch REQUIRED)
set(GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_DIR
    "${GHOSTTY_SOURCE_DIR}/src/shell-integration"
)
set(GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/shell-integration"
)
set(GHOSTTY_QT_SHELL_INTEGRATION_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/ghostty-shell-integration-ghostty-qt.patch"
)
set(GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ValidateGhosttyShellIntegration.cmake"
)
set(GHOSTTY_QT_INSTALL_RESOURCES_DIR "${CMAKE_INSTALL_DATADIR}/ghostty-qt")
set(GHOSTTY_QT_INSTALL_SHELL_INTEGRATION_DIR
    "${GHOSTTY_QT_INSTALL_RESOURCES_DIR}/shell-integration"
)
set(GHOSTTY_QT_INSTALL_THEMES_DIR "${GHOSTTY_QT_INSTALL_RESOURCES_DIR}/themes")
set(GHOSTTY_QT_SHELL_COMPLETION_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/dist/shell-completions"
)
if(NOT GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    string(APPEND GHOSTTY_QT_SHELL_COMPLETION_SOURCE_DIR "/config-disabled")
endif()
file(
    GLOB_RECURSE GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_FILES
    CONFIGURE_DEPENDS
    LIST_DIRECTORIES FALSE
    "${GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_DIR}/*"
)
set_property(
    DIRECTORY
    APPEND
    PROPERTY
        CMAKE_CONFIGURE_DEPENDS
            "${GHOSTTY_QT_SHELL_INTEGRATION_PATCH}"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageGhosttyShellIntegration.cmake"
            "${GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT}"
            ${GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_FILES}
)
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${GHOSTTY_QT_SHELL_INTEGRATION_SOURCE_DIR}"
        "-DSTAGE_DIR=${GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR}"
        "-DPATCH_EXECUTABLE=${GHOSTTY_QT_PATCH_EXECUTABLE}"
        "-DPATCH_FILE=${GHOSTTY_QT_SHELL_INTEGRATION_PATCH}"
        "-DVALIDATE_SCRIPT=${GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT}" -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageGhosttyShellIntegration.cmake"
    RESULT_VARIABLE GHOSTTY_QT_SHELL_INTEGRATION_STAGE_RESULT
)
if(NOT GHOSTTY_QT_SHELL_INTEGRATION_STAGE_RESULT EQUAL 0)
    message(
        FATAL_ERROR
        "Unable to stage the pinned Ghostty shell-integration resources"
    )
endif()

find_program(GHOSTTY_QT_TIC_EXECUTABLE tic REQUIRED)
set(GHOSTTY_QT_TERMINFO_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/terminfo/ghostty.terminfo"
)
set(GHOSTTY_QT_TERMINFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/share/terminfo")
set(GHOSTTY_QT_TERMINFO_STAMP
    "${CMAKE_CURRENT_BINARY_DIR}/terminfo/compiled.stamp"
)
set(GHOSTTY_QT_INSTALL_TERMINFO_DIR
    "${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo"
)

# Keep the compiled resource locator relocatable. It records only the path from
# the installed executable directory to the private terminfo database, never a
# configure-time build or installation prefix.
set(_ghostty_qt_install_bindir "${CMAKE_INSTALL_BINDIR}")
cmake_path(
    ABSOLUTE_PATH _ghostty_qt_install_bindir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_bindir_absolute
)
set(_ghostty_qt_install_resources_dir "${GHOSTTY_QT_INSTALL_RESOURCES_DIR}")
cmake_path(
    ABSOLUTE_PATH _ghostty_qt_install_resources_dir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_resources_dir_absolute
)
cmake_path(
    RELATIVE_PATH _ghostty_qt_install_resources_dir_absolute
    BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
    OUTPUT_VARIABLE GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR
)
set(_ghostty_qt_install_terminfo_dir "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")
cmake_path(
    ABSOLUTE_PATH _ghostty_qt_install_terminfo_dir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_terminfo_dir_absolute
)
cmake_path(
    RELATIVE_PATH _ghostty_qt_install_terminfo_dir_absolute
    BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
    OUTPUT_VARIABLE GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR
)
add_custom_command(
    OUTPUT "${GHOSTTY_QT_TERMINFO_STAMP}"
    COMMAND
        "${CMAKE_COMMAND}" -DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}
        -DGHOSTTY_SOURCE_DIR=${GHOSTTY_SOURCE_DIR}
        -DOUTPUT_FILE=${GHOSTTY_QT_TERMINFO_SOURCE} -P
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerminfo.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GHOSTTY_QT_TERMINFO_DIR}"
    COMMAND
        "${GHOSTTY_QT_TIC_EXECUTABLE}" -x -o "${GHOSTTY_QT_TERMINFO_DIR}"
        "${GHOSTTY_QT_TERMINFO_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${GHOSTTY_QT_TERMINFO_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerminfo.zig"
        "${GHOSTTY_SOURCE_DIR}/src/terminfo/Source.zig"
        "${GHOSTTY_SOURCE_DIR}/src/terminfo/ghostty.zig"
    COMMENT "Generating and compiling xterm-ghostty terminfo"
    VERBATIM
)
add_custom_target(ghostty-qt-terminfo DEPENDS "${GHOSTTY_QT_TERMINFO_STAMP}")
