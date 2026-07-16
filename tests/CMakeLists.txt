qt_add_executable(test-launch-options
    test_launch_options.cpp
    ../src/launch_options.cpp
    ../src/launch_options.h
)
target_include_directories(test-launch-options PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test-launch-options PRIVATE Qt6::Core Qt6::Test)
add_test(NAME launch-options COMMAND test-launch-options)

qt_add_executable(test-ghostty-smoke
    test_ghostty_smoke.cpp
)
target_link_libraries(test-ghostty-smoke PRIVATE Qt6::Core Qt6::Test ghostty-vt-static)
add_test(NAME ghostty-smoke COMMAND test-ghostty-smoke)

qt_add_executable(test-session-worker
    test_session_worker.cpp
    ../src/session_worker.cpp
    ../src/session_worker.h
    ../src/launch_options.h
    ../src/terminal_types.h
)
target_include_directories(test-session-worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test-session-worker PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(test-session-worker PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-runtime-paths
    ghostty-vt-static
    util
)
add_dependencies(test-session-worker ghostty-qt-terminfo)
add_test(NAME session-worker COMMAND test-session-worker)

qt_add_executable(test-terminal-pane
    test_terminal_pane.cpp
    ../src/session_worker.cpp
    ../src/session_worker.h
    ../src/terminal_controller.cpp
    ../src/terminal_controller.h
    ../src/terminal_pane.cpp
    ../src/terminal_pane.h
    ../src/launch_options.h
    ../src/terminal_types.h
)
target_include_directories(test-terminal-pane PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test-terminal-pane PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(test-terminal-pane PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-runtime-paths
    ghostty-vt-static
    util
)
add_dependencies(test-terminal-pane ghostty-qt-terminfo)
add_test(NAME terminal-pane-render COMMAND test-terminal-pane)
set_tests_properties(terminal-pane-render PROPERTIES
    TIMEOUT 10
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=1"
)

add_test(
    NAME application-lifecycle
    COMMAND $<TARGET_FILE:ghostty-qt> -- /bin/true
)
set_tests_properties(application-lifecycle PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

set_tests_properties(launch-options ghostty-smoke session-worker PROPERTIES
    TIMEOUT 20
)
set_tests_properties(session-worker PROPERTIES
    ENVIRONMENT "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR}"
)

qt_add_executable(test-terminfo-path-probe
    test_terminfo_path_probe.cpp
)
target_link_libraries(test-terminfo-path-probe PRIVATE
    Qt6::Core
    ghostty-qt-runtime-paths
)

if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}"
   AND NOT IS_ABSOLUTE "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")
    add_test(
        NAME terminfo-relocatable-install
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSTAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/terminfo-relocation-stage"
            "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DINSTALL_TERMINFO_DIR=${GHOSTTY_QT_INSTALL_TERMINFO_DIR}"
            "-DPROBE=$<TARGET_FILE:test-terminfo-path-probe>"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RunTerminfoRelocationTest.cmake"
    )
    set_tests_properties(terminfo-relocatable-install PROPERTIES TIMEOUT 30)
endif()
