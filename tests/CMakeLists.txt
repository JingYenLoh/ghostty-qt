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
    GHOSTTY_QT_TERMINFO_DIR="${GHOSTTY_QT_TERMINFO_DIR}"
)
target_link_libraries(test-session-worker PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
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
    GHOSTTY_QT_TERMINFO_DIR="${GHOSTTY_QT_TERMINFO_DIR}"
)
target_link_libraries(test-terminal-pane PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-vt-static
    util
)
add_dependencies(test-terminal-pane ghostty-qt-terminfo)
add_test(NAME terminal-pane-render COMMAND test-terminal-pane)
set_tests_properties(terminal-pane-render PROPERTIES
    TIMEOUT 10
    ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=1"
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
