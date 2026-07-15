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

set_tests_properties(launch-options ghostty-smoke session-worker PROPERTIES
    TIMEOUT 20
)
