# PTY/session lifecycle and runtime integration.

add_library(
    ghostty-qt-link-matcher
    STATIC
    src/session/ghostty_link_matcher.cpp
    src/session/ghostty_link_matcher.h
)
target_include_directories(
    ghostty-qt-link-matcher
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/zig/link_matcher"
)
target_link_libraries(
    ghostty-qt-link-matcher
    PUBLIC Qt6::Core
    PRIVATE ghostty-qt-link-matcher-zig
)
target_compile_options(
    ghostty-qt-link-matcher
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-pty-write-buffer
    src/session/pty_write_buffer.cpp
    src/session/pty_write_buffer.h
)
target_link_libraries(ghostty-qt-pty-write-buffer PUBLIC Qt6::Core)

add_library(
    ghostty-qt-runtime-paths
    STATIC
    src/session/terminfo_paths.cpp
    src/session/terminfo_paths.h
)
target_include_directories(
    ghostty-qt-runtime-paths
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-runtime-paths PUBLIC Qt6::Core)
target_compile_definitions(
    ghostty-qt-runtime-paths
    PRIVATE
        GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_TERMINFO_RELATIVE_DIR}"
)

add_library(
    ghostty-qt-shell-integration
    STATIC
    src/session/ghostty_shell_integration.cpp
    src/session/ghostty_shell_integration.h
    src/session/ghostty_shell_integration_p.h
)
target_include_directories(
    ghostty-qt-shell-integration
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-shell-integration PUBLIC Qt6::Core Qt6::Gui)
target_compile_definitions(
    ghostty-qt-shell-integration
    PRIVATE
        GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR}"
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    set(_ghostty_qt_config_runtime_install_file
        "${CMAKE_INSTALL_LIBDIR}/ghostty-qt/libghostty${CMAKE_SHARED_LIBRARY_SUFFIX}"
    )
    cmake_path(
        ABSOLUTE_PATH _ghostty_qt_config_runtime_install_file
        BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}"
        NORMALIZE
        OUTPUT_VARIABLE _ghostty_qt_config_runtime_install_file_absolute
    )
    cmake_path(
        RELATIVE_PATH _ghostty_qt_config_runtime_install_file_absolute
        BASE_DIRECTORY "${_ghostty_qt_install_bindir_absolute}"
        OUTPUT_VARIABLE GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_RELATIVE_TO_BINDIR
    )
    target_compile_definitions(
        ghostty-qt-shell-integration
        PRIVATE
            GHOSTTY_QT_CONFIG_HELPER_BUILD_PATH="$<TARGET_FILE:ghostty-qt-config-helper>"
            GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_BUILD_PATH="${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY}"
            GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_RELATIVE_TO_BINDIR="${GHOSTTY_QT_CONFIG_RUNTIME_LIBRARY_RELATIVE_TO_BINDIR}"
    )
endif()
target_compile_options(
    ghostty-qt-shell-integration
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-session-worker
    src/session/session_worker.cpp
    src/session/session_worker.h
    src/session/terminal_clipboard_bridge.h
    src/terminal/model/terminal_action_result.h
    src/terminal/model/terminal_clipboard_codepoint_map.h
    src/terminal/model/terminal_desktop_notification.h
    src/terminal/model/terminal_osc8_index.h
    src/terminal/model/terminal_path.h
    src/terminal/model/terminal_progress_report.h
    src/terminal/model/terminal_session_options.h
    src/terminal/model/terminal_types.h
    src/terminal/model/terminal_write_file_action.h
)
target_compile_definitions(
    ghostty-qt-session-worker
    PRIVATE GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(
    ghostty-qt-session-worker
    PUBLIC Qt6::Core Qt6::Gui ghostty-qt-pty-write-buffer
    PRIVATE
        ghostty-qt-action-pipeline
        ghostty-qt-desktop-activation
        ghostty-qt-key-identity
        ghostty-qt-linux-cgroup
        ghostty-qt-link-matcher
        ghostty-qt-posix-regular-file
        ghostty-qt-runtime-paths
        ghostty-qt-shell-integration
        ghostty-qt-terminal-foundation
        ghostty-qt-vt-adapter
        util
)

ghostty_qt_add_internal_library(ghostty-qt-terminal-controller
    src/session/terminal_controller.cpp
    src/session/terminal_controller.h
    src/terminal/model/terminal_desktop_notification.h
    src/terminal/model/terminal_progress_report.h
)
target_link_libraries(
    ghostty-qt-terminal-controller
    PUBLIC
        Qt6::Core
        Qt6::Gui
        ghostty-qt-session-worker
        ghostty-qt-terminal-foundation
)
