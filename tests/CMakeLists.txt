find_program(GHOSTTY_QT_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(Git_FOUND AND GHOSTTY_QT_CLANG_FORMAT_EXECUTABLE)
    add_test(NAME clang-format-hook
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/test_format_hook.sh"
            "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(clang-format-hook PROPERTIES
        TIMEOUT 20
        ENVIRONMENT
            "CLANG_FORMAT=${GHOSTTY_QT_CLANG_FORMAT_EXECUTABLE}"
    )
endif()

qt_add_executable(test-frontend-config
    test_frontend_config.cpp
)
target_include_directories(test-frontend-config PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-frontend-config PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-frontend-config
)
add_test(NAME frontend-config COMMAND test-frontend-config)
set_tests_properties(frontend-config PROPERTIES TIMEOUT 15)

qt_add_executable(test-launch-options
    test_launch_options.cpp
    ghostty_config_export_fixture.h
    ghostty_config_snapshot_fixture.h
    ../src/launch_options.cpp
    ../src/launch_options.h
    ../src/terminal_appearance.h
    ../src/terminal_session_options.h
)
target_include_directories(test-launch-options PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test-launch-options PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-config-export
    ghostty-qt-frontend-config
)
add_test(NAME launch-options COMMAND test-launch-options)

qt_add_executable(test-terminal-cell-metrics
    test_terminal_cell_metrics.cpp
    ../src/terminal_cell_metrics.cpp
    ../src/terminal_cell_metrics.h
)
target_include_directories(test-terminal-cell-metrics PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-cell-metrics PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME terminal-cell-metrics COMMAND test-terminal-cell-metrics)
set_tests_properties(terminal-cell-metrics PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-terminal-geometry
    test_terminal_geometry.cpp
    ../src/terminal_geometry.cpp
    ../src/terminal_geometry.h
    ../src/terminal_session_options.h
)
target_include_directories(test-terminal-geometry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-geometry PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME terminal-geometry COMMAND test-terminal-geometry)
set_tests_properties(terminal-geometry PROPERTIES TIMEOUT 10)

add_executable(fake-ghostty-cli-helper
    fake_ghostty_cli_helper.cpp
)
target_compile_options(fake-ghostty-cli-helper PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(pty-geometry-probe
    pty_geometry_probe.cpp
)
target_compile_options(pty-geometry-probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

qt_add_executable(test-ghostty-cli-delegation
    test_ghostty_cli_delegation.cpp
)
target_include_directories(test-ghostty-cli-delegation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-ghostty-cli-delegation PRIVATE
    GHOSTTY_QT_TEST_CONFIG_ENABLED=$<BOOL:${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}>
    GHOSTTY_QT_TEST_EXECUTABLE="$<TARGET_FILE:ghostty-qt>"
    GHOSTTY_QT_TEST_FAKE_HELPER="$<TARGET_FILE:fake-ghostty-cli-helper>"
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    target_compile_definitions(test-ghostty-cli-delegation PRIVATE
        GHOSTTY_QT_TEST_REAL_HELPER="$<TARGET_FILE:ghostty-qt-config-helper>"
    )
else()
    target_compile_definitions(test-ghostty-cli-delegation PRIVATE
        GHOSTTY_QT_TEST_REAL_HELPER=""
    )
endif()
target_link_libraries(test-ghostty-cli-delegation PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-cli-delegation
)
add_dependencies(test-ghostty-cli-delegation
    fake-ghostty-cli-helper
    ghostty-qt
)
add_test(NAME ghostty-cli-delegation
    COMMAND test-ghostty-cli-delegation)
set_tests_properties(ghostty-cli-delegation PROPERTIES
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

qt_add_executable(test-desktop-activation
    test_desktop_activation.cpp
)
target_include_directories(test-desktop-activation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-desktop-activation PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-desktop-activation
)
add_test(NAME desktop-activation COMMAND test-desktop-activation)
set_tests_properties(desktop-activation PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-application-lifetime
    test_application_lifetime.cpp
    ../src/application_lifetime.cpp
    ../src/application_lifetime.h
    ../src/launch_options.h
)
target_include_directories(test-application-lifetime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-application-lifetime PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME application-lifetime-controller
    COMMAND test-application-lifetime)
set_tests_properties(application-lifetime-controller PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-single-instance-activation
    test_single_instance_activation.cpp
    private_session_bus.h
    ../src/single_instance_activation.cpp
    ../src/single_instance_activation.h
)
target_include_directories(test-single-instance-activation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-single-instance-activation PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    ghostty-qt-desktop-activation
)
add_test(NAME single-instance-activation
    COMMAND test-single-instance-activation)
set_tests_properties(single-instance-activation PROPERTIES TIMEOUT 25)

qt_add_executable(test-application-controller
    test_application_controller.cpp
    ../src/application_action.h
    ../src/application_controller.cpp
    ../src/application_controller.h
    ../src/application_lifetime.cpp
    ../src/application_lifetime.h
    ../src/ghostty_application_keybindings.cpp
    ../src/ghostty_application_keybindings.h
    ../src/ghostty_global_shortcut_portal.cpp
    ../src/ghostty_global_shortcut_portal.h
    ../src/initial_session_coordinator.cpp
    ../src/initial_session_coordinator.h
    ../src/launch_options.cpp
    ../src/launch_options.h
    ../src/session_worker.cpp
    ../src/session_worker.h
    ../src/tab_list_model.cpp
    ../src/tab_list_model.h
    ../src/terminal_controller.cpp
    ../src/terminal_controller.h
    ../src/terminal_clipboard.cpp
    ../src/terminal_clipboard.h
    ../src/terminal_cell_metrics.cpp
    ../src/terminal_cell_metrics.h
    ../src/terminal_geometry.cpp
    ../src/terminal_geometry.h
    ../src/terminal_pane.cpp
    ../src/terminal_pane.h
    ../src/terminal_workspace.cpp
    ../src/terminal_workspace.h
    ../src/terminal_osc8_index.h
    ../src/terminal_session_options.h
    ../src/terminal_types.h
    ../src/workspace_action.h
    ../src/workspace_ids.h
)
target_include_directories(test-application-controller PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-application-controller PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
    GHOSTTY_QT_RENDER_TEST_PROBE=1
    GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE="$<TARGET_FILE:pty-geometry-probe>"
)
target_link_libraries(test-application-controller PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    PkgConfig::XKBCommon
    ghostty-qt-action-pipeline
    ghostty-qt-desktop-activation
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-terminal-bell
    ghostty-qt-vt-adapter
    util
)
add_dependencies(test-application-controller
    ghostty-qt-terminfo
    pty-geometry-probe
)
add_test(NAME application-controller COMMAND test-application-controller)
set_tests_properties(application-controller PROPERTIES
    TIMEOUT 30
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)

qt_add_executable(test-initial-session-coordinator
    test_initial_session_coordinator.cpp
    ../src/initial_session_coordinator.cpp
    ../src/initial_session_coordinator.h
)
target_include_directories(test-initial-session-coordinator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-initial-session-coordinator PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME initial-session-coordinator
    COMMAND test-initial-session-coordinator)
set_tests_properties(initial-session-coordinator PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-types
    test_terminal_types.cpp
    ../src/terminal_actions.h
    ../src/terminal_clipboard.h
    ../src/terminal_osc8_index.h
    ../src/terminal_session_options.h
    ../src/terminal_types.h
)
target_include_directories(test-terminal-types PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-types PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME terminal-types COMMAND test-terminal-types)
set_tests_properties(terminal-types PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-bell
    test_terminal_bell.cpp
)
target_include_directories(test-terminal-bell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-bell PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-terminal-bell
)
add_test(NAME terminal-bell COMMAND test-terminal-bell)
set_tests_properties(terminal-bell PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-smoke
    test_ghostty_smoke.cpp
)
target_link_libraries(test-ghostty-smoke PRIVATE Qt6::Core Qt6::Test ghostty-vt-static)
add_test(NAME ghostty-smoke COMMAND test-ghostty-smoke)

qt_add_executable(test-session-worker
    test_session_worker.cpp
    ../src/session_worker.cpp
    ../src/session_worker.h
    ../src/terminal_osc8_index.h
    ../src/terminal_session_options.h
    ../src/terminal_types.h
)
target_include_directories(test-session-worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test-session-worker PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
    GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE="$<TARGET_FILE:pty-geometry-probe>"
)
target_link_libraries(test-session-worker PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-action-pipeline
    ghostty-qt-desktop-activation
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-vt-adapter
    util
)
add_dependencies(test-session-worker
    ghostty-qt-terminfo
    pty-geometry-probe
)
add_test(NAME session-worker COMMAND test-session-worker)

qt_add_executable(test-terminal-pane
    test_terminal_pane.cpp
    ../src/initial_session_coordinator.cpp
    ../src/initial_session_coordinator.h
    ../src/session_worker.cpp
    ../src/session_worker.h
    ../src/terminal_controller.cpp
    ../src/terminal_controller.h
    ../src/terminal_clipboard.cpp
    ../src/terminal_clipboard.h
    ../src/terminal_cell_metrics.cpp
    ../src/terminal_cell_metrics.h
    ../src/terminal_geometry.cpp
    ../src/terminal_geometry.h
    ../src/terminal_pane.cpp
    ../src/terminal_pane.h
    ../src/terminal_pane_render_probe_p.h
    ../src/launch_options.cpp
    ../src/launch_options.h
    ../src/terminal_osc8_index.h
    ../src/terminal_session_options.h
    ../src/terminal_types.h
)
target_include_directories(test-terminal-pane PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test-terminal-pane PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
    GHOSTTY_QT_RENDER_TEST_PROBE=1
)
target_link_libraries(test-terminal-pane PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-action-pipeline
    ghostty-qt-desktop-activation
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-terminal-bell
    ghostty-qt-vt-adapter
    util
)
add_dependencies(test-terminal-pane ghostty-qt-terminfo)
add_test(NAME terminal-pane-render COMMAND test-terminal-pane)
set_tests_properties(terminal-pane-render PROPERTIES
    TIMEOUT 75
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=1"
)

qt_add_executable(test-terminal-workspace
    test_terminal_workspace.cpp
    ../src/ghostty_application_keybindings.cpp
    ../src/ghostty_application_keybindings.h
    ../src/ghostty_global_shortcut_portal.cpp
    ../src/ghostty_global_shortcut_portal.h
    ../src/initial_session_coordinator.cpp
    ../src/initial_session_coordinator.h
    ../src/launch_options.cpp
    ../src/launch_options.h
    ../src/session_worker.cpp
    ../src/session_worker.h
    ../src/tab_list_model.cpp
    ../src/tab_list_model.h
    ../src/terminal_controller.cpp
    ../src/terminal_controller.h
    ../src/terminal_clipboard.cpp
    ../src/terminal_clipboard.h
    ../src/terminal_cell_metrics.cpp
    ../src/terminal_cell_metrics.h
    ../src/terminal_geometry.cpp
    ../src/terminal_geometry.h
    ../src/terminal_pane.cpp
    ../src/terminal_pane.h
    ../src/terminal_osc8_index.h
    ../src/terminal_session_options.h
    ../src/terminal_workspace.cpp
    ../src/terminal_workspace.h
    ../src/terminal_types.h
    ../src/workspace_action.h
    ../src/workspace_ids.h
)
target_include_directories(test-terminal-workspace PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-terminal-workspace PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
    GHOSTTY_QT_RENDER_TEST_PROBE=1
)
target_link_libraries(test-terminal-workspace PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    PkgConfig::XKBCommon
    ghostty-qt-action-pipeline
    ghostty-qt-desktop-activation
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-terminal-bell
    ghostty-qt-vt-adapter
    util
)
add_dependencies(test-terminal-workspace ghostty-qt-terminfo)
add_test(NAME terminal-workspace COMMAND test-terminal-workspace)
set_tests_properties(terminal-workspace PROPERTIES
    TIMEOUT 60
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)
add_test(
    NAME terminal-workspace-divider-hidpi
    COMMAND test-terminal-workspace
            dragsExactNestedSplitDividerAndPreservesFocus
            splitDividerColorReloadsWithoutRelayout
            dimsUnfocusedSplitPanesAcrossLifecycle
            resizeOverlayIsPaneLocalAndScalesWithDpr
)
set_tests_properties(terminal-workspace-divider-hidpi PROPERTIES
    TIMEOUT 15
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=2;GHOSTTY_QT_EXPECT_HIDPI=1"
)

qt_add_executable(test-ghostty-vt-adapter
    test_ghostty_vt_adapter.cpp
)
target_link_libraries(test-ghostty-vt-adapter PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-vt-adapter
)
add_test(NAME ghostty-vt-adapter COMMAND test-ghostty-vt-adapter)
set_tests_properties(ghostty-vt-adapter PROPERTIES TIMEOUT 20)

qt_add_executable(test-ghostty-link-matcher
    test_ghostty_link_matcher.cpp
)
target_link_libraries(test-ghostty-link-matcher PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-link-matcher
)
add_test(NAME ghostty-link-matcher COMMAND test-ghostty-link-matcher)
set_tests_properties(ghostty-link-matcher PROPERTIES TIMEOUT 20)

add_test(NAME ghostty-link-matcher-upstream-corpus
    COMMAND "${CMAKE_COMMAND}"
        "-DZIG_EXECUTABLE=${GHOSTTY_QT_ZIG_EXECUTABLE}"
        "-DSOURCE_DIR=${GHOSTTY_QT_LINK_MATCHER_SOURCE_DIR}"
        "-DURL_SOURCE=${GHOSTTY_SOURCE_DIR}/src/config/url.zig"
        "-DCACHE_DIR=${GHOSTTY_QT_LINK_MATCHER_CACHE_DIR}"
        "-DGLOBAL_CACHE_DIR=${GHOSTTY_QT_ZIG_GLOBAL_CACHE_DIR}"
        "-DLOCK_FILE=${GHOSTTY_QT_LINK_MATCHER_REVISION_DIR}/build.lock"
        "-DOPTIMIZE=${_GHOSTTY_QT_LINK_MATCHER_OPTIMIZE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/TestGhosttyLinkMatcher.cmake"
)
set_tests_properties(ghostty-link-matcher-upstream-corpus PROPERTIES
    TIMEOUT 60)

qt_add_executable(test-workspace-foundation
    test_workspace_foundation.cpp
    ../src/tab_list_model.cpp
    ../src/tab_list_model.h
    ../src/workspace_action.h
    ../src/workspace_ids.h
)
target_include_directories(test-workspace-foundation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-workspace-foundation PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME workspace-foundation COMMAND test-workspace-foundation)
set_tests_properties(workspace-foundation PROPERTIES TIMEOUT 10)

qt_add_executable(test-tab-list-model
    test_tab_list_model.cpp
    ../src/tab_list_model.cpp
    ../src/tab_list_model.h
    ../src/workspace_ids.h
)
target_include_directories(test-tab-list-model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-tab-list-model PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME tab-list-model COMMAND test-tab-list-model)
set_tests_properties(tab-list-model PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-action-catalog
    test_ghostty_action_catalog.cpp
    ../src/workspace_action.h
    ../src/workspace_ids.h
)
target_link_libraries(test-ghostty-action-catalog PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-action-pipeline
)
add_test(NAME ghostty-action-catalog COMMAND test-ghostty-action-catalog)
set_tests_properties(ghostty-action-catalog PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-keybind-set
    test_ghostty_keybind_set.cpp
)
target_link_libraries(test-ghostty-keybind-set PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-action-pipeline
)
add_test(NAME ghostty-keybind-set COMMAND test-ghostty-keybind-set)
set_tests_properties(ghostty-keybind-set PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-global-shortcut-portal
    test_ghostty_global_shortcut_portal.cpp
    ../src/ghostty_global_shortcut_portal.cpp
    ../src/ghostty_global_shortcut_portal.h
)
target_include_directories(test-ghostty-global-shortcut-portal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-ghostty-global-shortcut-portal PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    PkgConfig::XKBCommon
)
add_test(NAME ghostty-global-shortcut-portal
    COMMAND test-ghostty-global-shortcut-portal)
set_tests_properties(ghostty-global-shortcut-portal PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-config-service
    test_ghostty_config_service.cpp
    ghostty_config_export_fixture.h
    ghostty_config_snapshot_fixture.h
    ../src/ghostty_config_loader.h
    ../src/ghostty_config_service.cpp
    ../src/ghostty_config_service.h
    ../src/ghostty_config_snapshot.h
)
target_include_directories(test-ghostty-config-service PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-ghostty-config-service PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-config-export
)
add_test(NAME ghostty-config-service COMMAND test-ghostty-config-service)
set_tests_properties(ghostty-config-service PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-config-edit
    test_ghostty_config_edit.cpp
    ../src/ghostty_config_edit.cpp
    ../src/ghostty_config_edit.h
)
target_include_directories(test-ghostty-config-edit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-ghostty-config-edit PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME ghostty-config-edit COMMAND test-ghostty-config-edit)
set_tests_properties(ghostty-config-edit PROPERTIES TIMEOUT 10)

add_executable(fake-ghostty-config-helper
    fake_ghostty_config_helper.cpp
)
target_compile_options(fake-ghostty-config-helper PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

qt_add_executable(test-ghostty-config-process-loader
    test_ghostty_config_process_loader.cpp
    ../src/ghostty_config_loader.h
    ../src/ghostty_config_process_loader.cpp
    ../src/ghostty_config_process_loader.h
    ../src/ghostty_config_snapshot.h
)
target_include_directories(test-ghostty-config-process-loader PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-ghostty-config-process-loader PRIVATE
    GHOSTTY_QT_FAKE_CONFIG_HELPER_PATH="$<TARGET_FILE:fake-ghostty-config-helper>"
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    target_compile_definitions(test-ghostty-config-process-loader PRIVATE
        GHOSTTY_QT_REAL_CONFIG_HELPER_PATH="$<TARGET_FILE:ghostty-qt-config-helper>"
    )
    add_dependencies(test-ghostty-config-process-loader
        ghostty-qt-config-helper)
else()
    target_compile_definitions(test-ghostty-config-process-loader PRIVATE
        GHOSTTY_QT_REAL_CONFIG_HELPER_PATH=""
    )
endif()
target_link_libraries(test-ghostty-config-process-loader PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-action-pipeline
    ghostty-qt-config-export
)
add_dependencies(test-ghostty-config-process-loader
    fake-ghostty-config-helper)
add_test(NAME ghostty-config-process-loader
    COMMAND test-ghostty-config-process-loader)
set_tests_properties(ghostty-config-process-loader PROPERTIES TIMEOUT 15)

qt_add_executable(test-ghostty-config-export
    test_ghostty_config_export.cpp
)
target_link_libraries(test-ghostty-config-export PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-config-export
)
add_test(NAME ghostty-config-export COMMAND test-ghostty-config-export)
set_tests_properties(ghostty-config-export PROPERTIES TIMEOUT 10)

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    set(_ghostty_qt_helper_test_home
        "${CMAKE_CURRENT_BINARY_DIR}/ghostty-config-helper-home")
    file(MAKE_DIRECTORY "${_ghostty_qt_helper_test_home}")
    add_test(NAME ghostty-config-helper-smoke
        COMMAND $<TARGET_FILE:ghostty-qt-config-helper> +validate-config)
    set_tests_properties(ghostty-config-helper-smoke PROPERTIES
        TIMEOUT 15
        ENVIRONMENT "XDG_CONFIG_HOME=${_ghostty_qt_helper_test_home}")
endif()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME ghostty-parity-manifest
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_parity_manifest.py"
)
set_tests_properties(ghostty-parity-manifest PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "GHOSTTY_QT_TEST_GHOSTTY_SOURCE_DIR=${GHOSTTY_SOURCE_DIR}"
)

add_test(
    NAME application-lifecycle
    COMMAND $<TARGET_FILE:ghostty-qt> -- /bin/true
)
set(_ghostty_qt_lifecycle_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-lifecycle-config")
file(MAKE_DIRECTORY "${_ghostty_qt_lifecycle_config_home}")
set_tests_properties(application-lifecycle PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_lifecycle_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

qt_add_executable(test-application-single-instance
    test_application_single_instance.cpp
    private_session_bus.h
)
target_include_directories(test-application-single-instance PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(test-application-single-instance PRIVATE
    GHOSTTY_QT_TEST_EXECUTABLE="$<TARGET_FILE:ghostty-qt>"
    GHOSTTY_QT_TEST_APPLICATION_ID="${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
    GHOSTTY_QT_TEST_CONFIG_ENABLED=$<BOOL:${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}>
)
target_link_libraries(test-application-single-instance PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
)
add_dependencies(test-application-single-instance ghostty-qt)
add_test(NAME application-single-instance
    COMMAND test-application-single-instance)
set_tests_properties(application-single-instance PROPERTIES
    TIMEOUT 60
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR}"
)

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    set(_ghostty_qt_resident_lifecycle_config_home
        "${CMAKE_CURRENT_BINARY_DIR}/application-resident-lifecycle-config")
    file(MAKE_DIRECTORY
        "${_ghostty_qt_resident_lifecycle_config_home}/ghostty"
        "${_ghostty_qt_resident_lifecycle_config_home}/ghostty-qt")
    file(WRITE
        "${_ghostty_qt_resident_lifecycle_config_home}/ghostty/config.ghostty"
        "quit-after-last-window-closed = false\nconfirm-close-surface = false\n")
    file(WRITE
        "${_ghostty_qt_resident_lifecycle_config_home}/ghostty-qt/config"
        "single-instance = false\n")
    foreach(_ghostty_qt_lifetime_mode IN ITEMS resident explicit-quit)
        add_test(
            NAME application-lifetime-${_ghostty_qt_lifetime_mode}
            COMMAND $<TARGET_FILE:ghostty-qt>
        )
        set_tests_properties(
            application-lifetime-${_ghostty_qt_lifetime_mode} PROPERTIES
            TIMEOUT 5
            ENVIRONMENT
                "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_APPLICATION_LIFETIME=${_ghostty_qt_lifetime_mode};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_resident_lifecycle_config_home}"
            FAIL_REGULAR_EXPRESSION
                "Binding loop detected;QQmlApplicationEngine failed"
        )
    endforeach()
endif()

add_test(
    NAME application-close-dialog
    COMMAND $<TARGET_FILE:ghostty-qt> -- /bin/sleep 30
)
set(_ghostty_qt_dialog_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-close-dialog-config")
file(MAKE_DIRECTORY "${_ghostty_qt_dialog_config_home}")
set_tests_properties(application-close-dialog PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_CONFIRM_CLOSE_DIALOG=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_dialog_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-tab-title-prompt
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_tab_title_prompt_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-tab-title-prompt-config")
file(MAKE_DIRECTORY "${_ghostty_qt_tab_title_prompt_config_home}")
set_tests_properties(application-tab-title-prompt PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_TAB_TITLE_PROMPT=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_tab_title_prompt_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-surface-title-prompt
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_surface_title_prompt_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-surface-title-prompt-config")
file(MAKE_DIRECTORY "${_ghostty_qt_surface_title_prompt_config_home}")
set_tests_properties(application-surface-title-prompt PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_SURFACE_TITLE_PROMPT=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_surface_title_prompt_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-fullscreen-action
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_fullscreen_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-fullscreen-config")
file(MAKE_DIRECTORY "${_ghostty_qt_fullscreen_config_home}")
set_tests_properties(application-fullscreen-action PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_TOGGLE_FULLSCREEN=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_fullscreen_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-maximize-action
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_maximize_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-maximize-config")
file(MAKE_DIRECTORY "${_ghostty_qt_maximize_config_home}")
set_tests_properties(application-maximize-action PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_TOGGLE_MAXIMIZE=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_maximize_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-window-decoration-action
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_window_decoration_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-window-decoration-config")
file(MAKE_DIRECTORY "${_ghostty_qt_window_decoration_config_home}")
set_tests_properties(application-window-decoration-action PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_TOGGLE_WINDOW_DECORATIONS=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_window_decoration_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    add_test(
        NAME application-initial-window-state
        COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
    )
    set(_ghostty_qt_initial_window_state_config_home
        "${CMAKE_CURRENT_BINARY_DIR}/application-initial-window-state-config")
    file(MAKE_DIRECTORY
        "${_ghostty_qt_initial_window_state_config_home}/ghostty"
        "${_ghostty_qt_initial_window_state_config_home}/ghostty-qt")
    file(WRITE
        "${_ghostty_qt_initial_window_state_config_home}/ghostty/config.ghostty"
        "confirm-close-surface = false\nwindow-width = 40\nwindow-height = 12\nmaximize = true\nfullscreen = true\n")
    file(WRITE
        "${_ghostty_qt_initial_window_state_config_home}/ghostty-qt/config"
        "single-instance = false\n")
    set_tests_properties(application-initial-window-state PROPERTIES
        TIMEOUT 5
        ENVIRONMENT
            "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_INITIAL_WINDOW_STATE=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_initial_window_state_config_home}"
        FAIL_REGULAR_EXPRESSION
            "Binding loop detected;QQmlApplicationEngine failed"
    )

    add_test(
        NAME application-initial-window-size
        COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
    )
    set(_ghostty_qt_initial_window_size_config_home
        "${CMAKE_CURRENT_BINARY_DIR}/application-initial-window-size-config")
    file(MAKE_DIRECTORY
        "${_ghostty_qt_initial_window_size_config_home}/ghostty"
        "${_ghostty_qt_initial_window_size_config_home}/ghostty-qt")
    file(WRITE
        "${_ghostty_qt_initial_window_size_config_home}/ghostty/config.ghostty"
        "confirm-close-surface = false\nwindow-width = 40\nwindow-height = 12\n")
    file(WRITE
        "${_ghostty_qt_initial_window_size_config_home}/ghostty-qt/config"
        "single-instance = false\n")
    set_tests_properties(application-initial-window-size PROPERTIES
        TIMEOUT 5
        ENVIRONMENT
            "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_INITIAL_WINDOW_SIZE=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;QT_SCALE_FACTOR=1;XDG_CONFIG_HOME=${_ghostty_qt_initial_window_size_config_home}"
        FAIL_REGULAR_EXPRESSION
            "Binding loop detected;QQmlApplicationEngine failed"
    )
endif()

add_test(
    NAME application-tab-bar-visibility
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_tab_bar_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-tab-bar-config")
file(MAKE_DIRECTORY "${_ghostty_qt_tab_bar_config_home}")
set_tests_properties(application-tab-bar-visibility PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_TAB_BAR_VISIBILITY=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_tab_bar_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-tabs-location
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_tabs_location_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-tabs-location-config")
file(MAKE_DIRECTORY
    "${_ghostty_qt_tabs_location_config_home}/ghostty-qt")
file(WRITE
    "${_ghostty_qt_tabs_location_config_home}/ghostty-qt/config"
    "tabs-location = bottom\nsingle-instance = false\n")
set_tests_properties(application-tabs-location PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_TABS_LOCATION=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_tabs_location_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

set_tests_properties(launch-options ghostty-smoke session-worker PROPERTIES
    TIMEOUT 20
)
set_tests_properties(session-worker PROPERTIES
    ENVIRONMENT "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR}"
)

add_test(
    NAME desktop-integration-install
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DSTAGE_DIR=${CMAKE_SOURCE_DIR}/tmp/desktop-integration-install-${CMAKE_BUILD_TYPE}-${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}"
        "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
        "-DINSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
        "-DAPPLICATION_ID=${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
        "-DEXPECT_CONFIG_HELPER=${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}"
        "-DCONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/RunDesktopIntegrationInstallTest.cmake"
)
set_tests_properties(desktop-integration-install PROPERTIES TIMEOUT 30)

qt_add_executable(test-terminfo-path-probe
    test_terminfo_path_probe.cpp
)
target_link_libraries(test-terminfo-path-probe PRIVATE
    Qt6::Core
    ghostty-qt-runtime-paths
)

if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}"
   AND NOT IS_ABSOLUTE "${GHOSTTY_QT_INSTALL_TERMINFO_DIR}")
    set(_ghostty_qt_relocation_config_helper_name "")
    if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
        set(_ghostty_qt_relocation_config_helper_name
            "$<TARGET_FILE_NAME:ghostty-qt-config-helper>")
    endif()
    add_test(
        NAME terminfo-relocatable-install
        COMMAND "${CMAKE_COMMAND}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DSTAGE_DIR=${CMAKE_CURRENT_BINARY_DIR}/terminfo-relocation-stage"
            "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
            "-DINSTALL_TERMINFO_DIR=${GHOSTTY_QT_INSTALL_TERMINFO_DIR}"
            "-DPROBE=$<TARGET_FILE:test-terminfo-path-probe>"
            "-DCONFIG_HELPER_NAME=${_ghostty_qt_relocation_config_helper_name}"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RunTerminfoRelocationTest.cmake"
    )
    set_tests_properties(terminfo-relocatable-install PROPERTIES TIMEOUT 30)
endif()
