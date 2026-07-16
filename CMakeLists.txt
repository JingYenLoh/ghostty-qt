cmake_minimum_required(VERSION 3.24)

project(ghostty-qt VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ghostty-qt supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

option(GHOSTTY_QT_BUILD_TESTS "Build ghostty-qt tests" ON)
option(GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY "Allow a Ghostty checkout other than the pinned revision" OFF)

set(GHOSTTY_QT_GHOSTTY_REVISION
    "c5a21edfcbc2d5b46540ad91b7980aca31f5f1f3"
    CACHE STRING "Expected Ghostty source revision")
set(GHOSTTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ghostty"
    CACHE PATH "Path to the pinned Ghostty checkout")

if(NOT EXISTS "${GHOSTTY_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Ghostty source not found at ${GHOSTTY_SOURCE_DIR}. "
        "Initialize the pinned ghostty submodule first.")
endif()

find_package(Git QUIET)
if(Git_FOUND AND EXISTS "${GHOSTTY_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${GHOSTTY_SOURCE_DIR}"
        OUTPUT_VARIABLE GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE GHOSTTY_QT_GIT_RESULT)
    if(GHOSTTY_QT_GIT_RESULT EQUAL 0
       AND NOT GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION STREQUAL GHOSTTY_QT_GHOSTTY_REVISION
       AND NOT GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY)
        message(FATAL_ERROR
            "Ghostty is at ${GHOSTTY_QT_ACTUAL_GHOSTTY_REVISION}, expected "
            "${GHOSTTY_QT_GHOSTTY_REVISION}. Pass "
            "-DGHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY=ON only when intentionally testing an upgrade.")
    endif()
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.8)

# Keep the Zig-built engine identical between Debug and Release C++ builds.
# Ghostty writes both configurations into its source-tree zig-out directory,
# so developer and release builds must not be run concurrently.
set(_GHOSTTY_QT_DEFAULT_ZIG_FLAGS "")
if(NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    set(_GHOSTTY_QT_DEFAULT_ZIG_FLAGS "-Doptimize=ReleaseFast")
endif()
set(GHOSTTY_ZIG_BUILD_FLAGS "${_GHOSTTY_QT_DEFAULT_ZIG_FLAGS}" CACHE STRING
    "Additional flags passed to Ghostty's Zig build")

find_program(GHOSTTY_QT_ZIG_EXECUTABLE zig)
if(NOT GHOSTTY_QT_ZIG_EXECUTABLE)
    message(FATAL_ERROR
        "Zig 0.15.2 was not found on PATH. Install that exact toolchain "
        "before configuring ghostty-qt.")
endif()
execute_process(
    COMMAND "${GHOSTTY_QT_ZIG_EXECUTABLE}" version
    OUTPUT_VARIABLE GHOSTTY_QT_ZIG_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE GHOSTTY_QT_ZIG_VERSION_RESULT)
if(NOT GHOSTTY_QT_ZIG_VERSION_RESULT EQUAL 0 OR NOT GHOSTTY_QT_ZIG_VERSION STREQUAL "0.15.2")
    message(FATAL_ERROR
        "The pinned Ghostty revision requires Zig 0.15.2; found "
        "'${GHOSTTY_QT_ZIG_VERSION}' at ${GHOSTTY_QT_ZIG_EXECUTABLE}.")
endif()

# Ghostty's wrapper invokes Zig and exports the ghostty-vt-static target.
add_subdirectory("${GHOSTTY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ghostty" EXCLUDE_FROM_ALL)

# libghostty-vt has public C struct fields named `emit`. Disabling Qt's
# convenience keywords avoids a preprocessor collision with that valid API.
add_compile_definitions(QT_NO_KEYWORDS)

find_program(GHOSTTY_QT_TIC_EXECUTABLE tic REQUIRED)
set(GHOSTTY_QT_TERMINFO_SOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/terminfo/ghostty.terminfo")
set(GHOSTTY_QT_TERMINFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/share/terminfo")
set(GHOSTTY_QT_TERMINFO_STAMP "${CMAKE_CURRENT_BINARY_DIR}/terminfo/compiled.stamp")
set(GHOSTTY_QT_INSTALL_TERMINFO_DIR
    "${CMAKE_INSTALL_DATADIR}/ghostty-qt/terminfo")

# Keep the compiled resource locator relocatable. It records only the path from
# the installed executable directory to the private terminfo database, never a
# configure-time build or installation prefix.
set(_ghostty_qt_install_bindir "${CMAKE_INSTALL_BINDIR}")
cmake_path(ABSOLUTE_PATH _ghostty_qt_install_bindir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_bindir_absolute)
set(_ghostty_qt_install_terminfo_dir "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")
cmake_path(ABSOLUTE_PATH _ghostty_qt_install_terminfo_dir
    BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
    NORMALIZE
    OUTPUT_VARIABLE _ghostty_qt_install_terminfo_dir_absolute)
cmake_path(RELATIVE_PATH _ghostty_qt_install_terminfo_dir_absolute
    BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
    OUTPUT_VARIABLE GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR)
add_custom_command(
    OUTPUT "${GHOSTTY_QT_TERMINFO_STAMP}"
    COMMAND "${CMAKE_COMMAND}"
        -DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}
        -DGHOSTTY_SOURCE_DIR=${GHOSTTY_SOURCE_DIR}
        -DOUTPUT_FILE=${GHOSTTY_QT_TERMINFO_SOURCE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerminfo.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GHOSTTY_QT_TERMINFO_DIR}"
    COMMAND "${GHOSTTY_QT_TIC_EXECUTABLE}" -x -o "${GHOSTTY_QT_TERMINFO_DIR}"
        "${GHOSTTY_QT_TERMINFO_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${GHOSTTY_QT_TERMINFO_STAMP}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateTerminfo.zig"
        "${GHOSTTY_SOURCE_DIR}/src/terminfo/Source.zig"
        "${GHOSTTY_SOURCE_DIR}/src/terminfo/ghostty.zig"
    COMMENT "Generating and compiling xterm-ghostty terminfo"
    VERBATIM)
add_custom_target(ghostty-qt-terminfo DEPENDS "${GHOSTTY_QT_TERMINFO_STAMP}")

add_library(ghostty-qt-runtime-paths STATIC
    src/terminfo_paths.cpp
    src/terminfo_paths.h
)
target_include_directories(ghostty-qt-runtime-paths PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-runtime-paths PUBLIC Qt6::Core)
target_compile_definitions(ghostty-qt-runtime-paths PRIVATE
    GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR}"
)

qt_add_executable(ghostty-qt
    src/main.cpp
    src/launch_options.cpp
    src/launch_options.h
    src/session_worker.cpp
    src/session_worker.h
    src/terminal_controller.cpp
    src/terminal_controller.h
    src/terminal_pane.cpp
    src/terminal_pane.h
    src/terminal_types.h
    src/terminal_workspace.cpp
    src/terminal_workspace.h
)

qt_add_qml_module(ghostty-qt
    URI GhosttyQt
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(ghostty-qt PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    ghostty-qt-runtime-paths
    ghostty-vt-static
    util
)

target_compile_definitions(ghostty-qt PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
)

add_dependencies(ghostty-qt ghostty-qt-terminfo)

target_compile_options(ghostty-qt PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

install(TARGETS ghostty-qt RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${GHOSTTY_QT_TERMINFO_DIR}/"
    DESTINATION "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")

if(GHOSTTY_QT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()
