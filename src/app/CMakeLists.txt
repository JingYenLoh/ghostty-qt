# Application policy, startup, composition, and the QML executable.

ghostty_qt_add_internal_library(ghostty-qt-application-identity
    src/app/application_identity.cpp
    src/app/application_identity.h
)
target_link_libraries(ghostty-qt-application-identity PUBLIC Qt6::Core)

add_library(
    ghostty-qt-cli-delegation
    STATIC
    src/app/ghostty_cli_delegation.cpp
    src/app/ghostty_cli_delegation.h
)
target_include_directories(
    ghostty-qt-cli-delegation
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_options(
    ghostty-qt-cli-delegation
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-application-ipc
    src/app/ghostty_application_ipc.cpp
    src/app/ghostty_application_ipc.h
)
target_link_libraries(
    ghostty-qt-application-ipc
    PUBLIC Qt6::Core Qt6::DBus ghostty-qt-application-identity
)

ghostty_qt_add_internal_library(ghostty-qt-launch-options
    src/app/application_appearance.cpp
    src/app/application_appearance.h
    src/app/launch_options.cpp
    src/app/launch_options.h
    src/terminal/model/terminal_appearance.h
    src/terminal/model/terminal_clipboard_codepoint_map.h
    src/terminal/model/terminal_color_scheme.h
    src/terminal/model/terminal_custom_shader_options.h
    src/terminal/model/terminal_path.h
    src/terminal/model/terminal_session_options.h
)
target_link_libraries(
    ghostty-qt-launch-options
    PUBLIC
        Qt6::Core
        Qt6::Gui
        ghostty-qt-config-export
        ghostty-qt-frontend-config
)

ghostty_qt_add_internal_library(ghostty-qt-application-lifetime
    src/app/application_lifetime.cpp
    src/app/application_lifetime.h
)
target_link_libraries(
    ghostty-qt-application-lifetime
    PUBLIC Qt6::Core Qt6::Gui ghostty-qt-launch-options
)

ghostty_qt_add_internal_library(ghostty-qt-application-controller
    src/app/application_controller.cpp
    src/app/application_controller.h
)
target_link_libraries(
    ghostty-qt-application-controller
    PUBLIC
        Qt6::Core
        ghostty-qt-application-lifetime
        ghostty-qt-desktop-activation
        ghostty-qt-launch-options
    PRIVATE
        Qt6::DBus
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        ghostty-qt-application-ipc
        ghostty-qt-application-keybindings
        ghostty-qt-desktop-notification
        ghostty-qt-quick-terminal
        ghostty-qt-terminal-foundation
        ghostty-qt-window-ui
        ghostty-qt-window-effects
)

qt_add_executable(ghostty-qt
    src/app/main.cpp
    src/app/application_identity.h
    src/app/application_action.h
    src/config/ghostty_config_export.h
    src/config/ghostty_config_values.h
    src/config/ghostty_keybind_config.h
    src/input/key_event_context.h
    src/input/key_event_snapshot.h
    src/input/modifier_remap_types.h
    src/desktop/quick_terminal.h
    src/desktop/quick_terminal_surface.h
    src/support/revision_counter.h
    src/terminal/model/terminal_action_result.h
    src/terminal/model/terminal_appearance.h
    src/terminal/model/terminal_command.h
    src/terminal/model/terminal_custom_shader_qml.h
    src/terminal/model/terminal_custom_shader_options.h
    src/terminal/model/terminal_osc8_index.h
    src/workspace/terminal_pane.cpp
    src/workspace/terminal_pane.h
    src/workspace/terminal_pane_renderer.cpp
    src/workspace/terminal_pane_renderer.h
    src/terminal/model/terminal_path.h
    src/terminal/model/terminal_session_options.h
    src/terminal/model/terminal_initial_input.h
    src/terminal/inspector/terminal_inspector_event_model.cpp
    src/terminal/inspector/terminal_inspector_event_model.h
    src/terminal/inspector/terminal_inspector_model.cpp
    src/terminal/inspector/terminal_inspector_model.h
    src/terminal/model/terminal_typography.h
    src/terminal/model/terminal_types.h
    src/terminal/model/terminal_write_file_action.h
    src/workspace/terminal_workspace.cpp
    src/workspace/terminal_workspace.h
    src/workspace/window_navigation_action.h
    src/workspace/workspace_action.h
    src/workspace/workspace_ids.h
)

qt_add_qml_module(ghostty-qt
    URI GhosttyQt
    VERSION 1.0
    QML_FILES
        qml/AbnormalExitOverlay.qml
        qml/BellBorderOverlay.qml
        qml/ChromeToolButton.qml
        qml/CommandPalette.qml
        qml/ConfigDiagnosticsDialog.qml
        qml/KeyStateOverlay.qml
        qml/Main.qml
        qml/ReadOnlyOverlay.qml
        qml/ResizeOverlay.qml
        qml/ProgressOverlay.qml
        qml/SearchOverlay.qml
        qml/TabOverview.qml
        qml/TerminalScrollBar.qml
        qml/TerminalInspector.qml
        qml/TerminalShaderStage.qml
        qml/AppToast.qml
    RESOURCES
        qml/icons/cell-pick.svg
        qml/icons/go-down.svg
        qml/icons/go-up.svg
        qml/icons/split-down.svg
        qml/icons/split-right.svg
        qml/icons/tab-new.svg
        qml/icons/view-refresh.svg
        qml/icons/window-close.svg
)

# qmltyperegistrar records a QML type's input header by basename and emits
# basename-only probe includes. Keep these generator-only lookup directories
# private; handwritten project includes remain rooted and domain-qualified.
target_include_directories(
    ghostty-qt
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/terminal/inspector"
        "${PROJECT_SOURCE_DIR}/src/terminal/model"
        "${PROJECT_SOURCE_DIR}/src/workspace"
)

target_link_libraries(
    ghostty-qt
    PRIVATE
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Widgets
        PkgConfig::XKBCommon
        ghostty-qt-action-pipeline
        ghostty-qt-application-identity
        ghostty-qt-application-ipc
        ghostty-qt-application-controller
        ghostty-qt-cli-delegation
        ghostty-qt-config-export
        ghostty-qt-config-runtime
        ghostty-qt-desktop-activation
        ghostty-qt-desktop-quick-controls-style
        ghostty-qt-frontend-config
        ghostty-qt-key-identity
        ghostty-qt-key-event-context
        ghostty-qt-keyboard-layout
        ghostty-qt-launch-options
        ghostty-qt-linux-cgroup
        ghostty-qt-link-matcher
        ghostty-qt-quick-terminal
        ghostty-qt-renderer-graphics-library-manifest
        ghostty-qt-runtime-paths
        ghostty-qt-single-instance-activation
        ghostty-qt-shell-integration
        ghostty-qt-systemd-notify
        ghostty-qt-tab-list-model
        ghostty-qt-terminal-controller
        ghostty-qt-terminal-foundation
        ghostty-qt-terminal-rendering
        ghostty-qt-terminal-bell
        ghostty-qt-vt-adapter
        ghostty-qt-window-ui
        util
)

target_compile_definitions(
    ghostty-qt
    PRIVATE
        GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
        GHOSTTY_QT_APPLICATION_ID="${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
        GHOSTTY_QT_CONFIG_ENABLED=$<BOOL:${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}>
)

add_dependencies(ghostty-qt ghostty-qt-terminfo)

target_compile_options(
    ghostty-qt
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
