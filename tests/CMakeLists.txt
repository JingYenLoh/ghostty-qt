qt_add_executable(test-launch-options
    test_launch_options.cpp
    ../src/launch_options.cpp
    ../src/launch_options.h
    ../src/terminal_appearance.h
    ../src/terminal_session_options.h
)
target_include_directories(test-launch-options PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test-launch-options PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME launch-options COMMAND test-launch-options)

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
)
target_link_libraries(test-session-worker PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-action-pipeline
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-vt-adapter
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
    ../src/terminal_clipboard.cpp
    ../src/terminal_clipboard.h
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
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-vt-adapter
    util
)
add_dependencies(test-terminal-pane ghostty-qt-terminfo)
add_test(NAME terminal-pane-render COMMAND test-terminal-pane)
set_tests_properties(terminal-pane-render PROPERTIES
    TIMEOUT 20
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=1"
)

qt_add_executable(test-terminal-workspace
    test_terminal_workspace.cpp
    ../src/ghostty_application_keybindings.cpp
    ../src/ghostty_application_keybindings.h
    ../src/ghostty_global_shortcut_portal.cpp
    ../src/ghostty_global_shortcut_portal.h
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
    ghostty-qt-link-matcher
    ghostty-qt-runtime-paths
    ghostty-qt-vt-adapter
    util
)
add_dependencies(test-terminal-workspace ghostty-qt-terminfo)
add_test(NAME terminal-workspace COMMAND test-terminal-workspace)
set_tests_properties(terminal-workspace PROPERTIES
    TIMEOUT 30
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)
add_test(
    NAME terminal-workspace-divider-hidpi
    COMMAND test-terminal-workspace
            dragsExactNestedSplitDividerAndPreservesFocus
            splitDividerColorReloadsWithoutRelayout
            dimsUnfocusedSplitPanesAcrossLifecycle
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
    Qt6::Test
)
add_test(NAME ghostty-config-service COMMAND test-ghostty-config-service)
set_tests_properties(ghostty-config-service PROPERTIES TIMEOUT 10)

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
)
add_dependencies(test-ghostty-config-process-loader
    fake-ghostty-config-helper)
add_test(NAME ghostty-config-process-loader
    COMMAND test-ghostty-config-process-loader)
set_tests_properties(ghostty-config-process-loader PROPERTIES TIMEOUT 15)

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
