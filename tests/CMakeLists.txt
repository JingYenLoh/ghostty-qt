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

add_test(NAME shell-integration-resources
    COMMAND "${CMAKE_COMMAND}"
        "-DSHELL_INTEGRATION_DIR=${GHOSTTY_QT_SHELL_INTEGRATION_STAGE_DIR}"
        -P "${GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT}")
set_tests_properties(shell-integration-resources PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-shell-integration
    test_ghostty_shell_integration.cpp
)
target_include_directories(test-ghostty-shell-integration PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-ghostty-shell-integration PRIVATE
    GHOSTTY_QT_TEST_INSTALL_RESOURCES_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR}"
)
target_link_libraries(test-ghostty-shell-integration PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-shell-integration
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    target_compile_definitions(test-ghostty-shell-integration PRIVATE
        GHOSTTY_QT_TEST_REAL_HELPER="$<TARGET_FILE:ghostty-qt-config-helper>"
        GHOSTTY_QT_TEST_RESOURCES="${CMAKE_BINARY_DIR}"
    )
    add_dependencies(test-ghostty-shell-integration
        ghostty-qt-config-helper)

    qt_add_executable(shell-integration-cache-probe
        shell_integration_cache_probe.cpp
    )
    target_include_directories(shell-integration-cache-probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(shell-integration-cache-probe PRIVATE
        Qt6::Core
        ghostty-qt-shell-integration
    )
    set(_ghostty_qt_shell_integration_cache_probe
        "$<TARGET_FILE:shell-integration-cache-probe>")
else()
    set(_ghostty_qt_shell_integration_cache_probe "")
endif()
add_test(NAME ghostty-shell-integration
    COMMAND test-ghostty-shell-integration)
set_tests_properties(ghostty-shell-integration PROPERTIES TIMEOUT 20)

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

qt_add_executable(test-desktop-quick-controls-style
    test_desktop_quick_controls_style.cpp
)
target_link_libraries(test-desktop-quick-controls-style PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-desktop-quick-controls-style
)
add_test(NAME desktop-quick-controls-style
    COMMAND test-desktop-quick-controls-style)

qt_add_executable(test-pty-write-buffer
    test_pty_write_buffer.cpp
)
target_link_libraries(test-pty-write-buffer PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-pty-write-buffer
)
add_test(NAME pty-write-buffer COMMAND test-pty-write-buffer)
set_tests_properties(pty-write-buffer PROPERTIES TIMEOUT 10)

qt_add_executable(test-keyboard-layout
    test_keyboard_layout.cpp
)
target_link_libraries(test-keyboard-layout PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    PkgConfig::XKBCommon
    ghostty-qt-keyboard-layout
)
add_test(NAME keyboard-layout COMMAND test-keyboard-layout)
set_tests_properties(keyboard-layout PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-key-identity
    test_ghostty_key_identity.cpp
)
target_link_libraries(test-ghostty-key-identity PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-key-identity
)
add_test(NAME ghostty-key-identity COMMAND test-ghostty-key-identity)
set_tests_properties(ghostty-key-identity PROPERTIES TIMEOUT 10)

qt_add_executable(test-launch-options
    test_launch_options.cpp
    ghostty_config_export_fixture.h
    ghostty_config_snapshot_fixture.h
)
target_include_directories(test-launch-options PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test-launch-options PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-config-export
    ghostty-qt-frontend-config
    ghostty-qt-launch-options
)
add_test(NAME launch-options COMMAND test-launch-options)

qt_add_executable(test-terminal-custom-shader-compiler
    test_terminal_custom_shader_compiler.cpp
)
target_include_directories(test-terminal-custom-shader-compiler PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-custom-shader-compiler PRIVATE
    Qt6::Core
    Qt6::GuiPrivate
    Qt6::ShaderToolsPrivate
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-custom-shader-compiler
    COMMAND test-terminal-custom-shader-compiler)
set_tests_properties(terminal-custom-shader-compiler PROPERTIES TIMEOUT 30)

qt_add_executable(test-renderdoc-capture
    test_renderdoc_capture.cpp
    renderdoc_capture.cpp
    renderdoc_capture.h
)
target_link_libraries(test-renderdoc-capture PRIVATE
    Qt6::Core
    Qt6::Test
    ${CMAKE_DL_LIBS}
)
add_test(NAME renderdoc-capture COMMAND test-renderdoc-capture)
set_tests_properties(renderdoc-capture PROPERTIES TIMEOUT 10)

qt_add_executable(test-renderer-graphics-library-manifest
    test_renderer_graphics_library_manifest.cpp
)
target_link_libraries(test-renderer-graphics-library-manifest PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-renderer-graphics-library-manifest
)
add_test(NAME renderer-graphics-library-manifest
    COMMAND test-renderer-graphics-library-manifest)
set_tests_properties(renderer-graphics-library-manifest PROPERTIES TIMEOUT 20)

qt_add_executable(test-terminal-custom-shader-qsg
    test_terminal_custom_shader_qsg.cpp
)
target_include_directories(test-terminal-custom-shader-qsg PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-custom-shader-qsg PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-custom-shader-qsg
    COMMAND test-terminal-custom-shader-qsg)
set_tests_properties(terminal-custom-shader-qsg PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)

qt_add_executable(test-terminal-custom-shader-rhi
    test_terminal_custom_shader_rhi.cpp
)
target_include_directories(test-terminal-custom-shader-rhi PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-custom-shader-rhi PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-custom-shader-rhi
    COMMAND test-terminal-custom-shader-rhi)
set_tests_properties(terminal-custom-shader-rhi PROPERTIES
    TIMEOUT 30
    ENVIRONMENT
        "LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
    SKIP_REGULAR_EXPRESSION
        "SKIP.*platform cannot initialize the requested RHI context"
)
add_test(NAME terminal-custom-shader-vulkan-rhi
    COMMAND test-terminal-custom-shader-rhi)
set_tests_properties(terminal-custom-shader-vulkan-rhi PROPERTIES
    TIMEOUT 30
    ENVIRONMENT
        "GHOSTTY_QT_SHADER_TEST_BACKEND=vulkan;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=vulkan;QT_SCALE_FACTOR=1"
    SKIP_REGULAR_EXPRESSION
        "SKIP.*platform cannot initialize the requested RHI context"
)

qt_add_executable(test-application-identity
    test_application_identity.cpp
)
target_include_directories(test-application-identity PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-application-identity PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-application-identity
)
add_test(NAME application-identity COMMAND test-application-identity)
set_tests_properties(application-identity PROPERTIES TIMEOUT 10)

qt_add_executable(test-systemd-notify
    test_systemd_notify.cpp
)
target_include_directories(test-systemd-notify PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-systemd-notify PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-systemd-notify
)
add_test(NAME systemd-notify COMMAND test-systemd-notify)
set_tests_properties(systemd-notify PROPERTIES TIMEOUT 10)

qt_add_executable(test-quick-terminal
    test_quick_terminal.cpp
)
target_include_directories(test-quick-terminal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-quick-terminal PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-quick-terminal
)
add_test(NAME quick-terminal COMMAND test-quick-terminal)
set_tests_properties(quick-terminal PROPERTIES TIMEOUT 10)

qt_add_executable(test-quick-terminal-surface
    test_quick_terminal_surface.cpp
)
target_include_directories(test-quick-terminal-surface PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-quick-terminal-surface PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-quick-terminal
)
add_test(NAME quick-terminal-surface COMMAND test-quick-terminal-surface)
set_tests_properties(quick-terminal-surface PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-window-blur-controller
    test_window_blur_controller.cpp
)
target_include_directories(test-window-blur-controller PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-window-blur-controller PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-window-effects
)
add_test(NAME window-blur-controller COMMAND test-window-blur-controller)
set_tests_properties(window-blur-controller PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-window-ui-controller
    test_window_ui_controller.cpp
)
target_include_directories(test-window-ui-controller PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-window-ui-controller PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-window-ui
)
add_test(NAME window-ui-controller COMMAND test-window-ui-controller)
set_tests_properties(window-ui-controller PROPERTIES TIMEOUT 10)

qt_add_executable(test-modifier-remap
    test_modifier_remap.cpp
)
target_include_directories(test-modifier-remap PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-modifier-remap PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-application-keybindings
    ghostty-qt-modifier-remap
    ghostty-qt-render-test-support
)
add_test(NAME modifier-remap COMMAND test-modifier-remap)
set_tests_properties(modifier-remap PROPERTIES TIMEOUT 10)

qt_add_executable(test-application-appearance
    test_application_appearance.cpp
)
target_include_directories(test-application-appearance PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-application-appearance PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-launch-options
)
add_test(NAME application-appearance COMMAND test-application-appearance)
set_tests_properties(application-appearance PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-cell-metrics
    test_terminal_cell_metrics.cpp
)
target_include_directories(test-terminal-cell-metrics PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-cell-metrics PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-terminal-foundation
)
add_test(NAME terminal-cell-metrics COMMAND test-terminal-cell-metrics)
set_tests_properties(terminal-cell-metrics PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-terminal-text-runs
    test_terminal_text_runs.cpp
)
target_include_directories(test-terminal-text-runs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-text-runs PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-terminal-foundation
)
add_test(NAME terminal-text-runs COMMAND test-terminal-text-runs)
set_tests_properties(terminal-text-runs PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-terminal-glyph-plan
    test_terminal_glyph_plan.cpp
)
target_include_directories(test-terminal-glyph-plan PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-glyph-plan PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-terminal-foundation
)
add_test(NAME terminal-glyph-plan COMMAND test-terminal-glyph-plan)
set_tests_properties(terminal-glyph-plan PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-terminal-geometry
    test_terminal_geometry.cpp
)
target_include_directories(test-terminal-geometry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-geometry PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-terminal-foundation
)
add_test(NAME terminal-geometry COMMAND test-terminal-geometry)
set_tests_properties(terminal-geometry PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-backdrop
    test_terminal_backdrop.cpp
)
target_include_directories(test-terminal-backdrop PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-backdrop PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-backdrop COMMAND test-terminal-backdrop)
set_tests_properties(terminal-backdrop PROPERTIES TIMEOUT 30)

qt_add_executable(test-terminal-alpha-blending
    test_terminal_alpha_blending.cpp
)
target_include_directories(test-terminal-alpha-blending PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-alpha-blending PRIVATE
    Qt6::Core
    Qt6::GuiPrivate
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-alpha-blending COMMAND test-terminal-alpha-blending)
set_tests_properties(terminal-alpha-blending PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-kitty-graphics
    test_terminal_kitty_graphics.cpp
)
target_include_directories(test-terminal-kitty-graphics PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-kitty-graphics PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-kitty-graphics COMMAND test-terminal-kitty-graphics)
set_tests_properties(terminal-kitty-graphics PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-terminal-backdrop-rhi
    test_terminal_backdrop_rhi.cpp
)
target_include_directories(test-terminal-backdrop-rhi PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-backdrop-rhi PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::GuiPrivate
    Qt6::Quick
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-backdrop-rhi COMMAND test-terminal-backdrop-rhi)
set_tests_properties(terminal-backdrop-rhi PROPERTIES
    TIMEOUT 20
    ENVIRONMENT
        "LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
    SKIP_REGULAR_EXPRESSION
        "SKIP.*platform cannot initialize the requested RHI context"
)
add_test(NAME terminal-backdrop-vulkan-rhi COMMAND test-terminal-backdrop-rhi)
set_tests_properties(terminal-backdrop-vulkan-rhi PROPERTIES
    TIMEOUT 20
    ENVIRONMENT
        "GHOSTTY_QT_BACKDROP_TEST_BACKEND=vulkan;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=vulkan;QT_SCALE_FACTOR=1"
    SKIP_REGULAR_EXPRESSION
        "SKIP.*platform cannot initialize the requested RHI context"
)

qt_add_executable(test-terminal-rect-batch
    test_terminal_rect_batch.cpp
)
target_include_directories(test-terminal-rect-batch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-rect-batch PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-rect-batch COMMAND test-terminal-rect-batch)
set_tests_properties(terminal-rect-batch PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-glyph-atlas
    test_terminal_glyph_atlas.cpp
)
target_include_directories(test-terminal-glyph-atlas PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-glyph-atlas PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-glyph-atlas COMMAND test-terminal-glyph-atlas)
set_tests_properties(terminal-glyph-atlas PROPERTIES
    TIMEOUT 10
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

qt_add_executable(test-terminal-glyph-batch
    test_terminal_glyph_batch.cpp
)
target_include_directories(test-terminal-glyph-batch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-glyph-batch PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-glyph-batch COMMAND test-terminal-glyph-batch)
set_tests_properties(terminal-glyph-batch PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-glyph-batch-rhi
    test_terminal_glyph_batch_rhi.cpp
)
target_include_directories(test-terminal-glyph-batch-rhi PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-glyph-batch-rhi PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-terminal-rendering
)
add_test(NAME terminal-glyph-batch-rhi COMMAND test-terminal-glyph-batch-rhi)
set_tests_properties(terminal-glyph-batch-rhi PROPERTIES
    TIMEOUT 20
    ENVIRONMENT
        "LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
    SKIP_REGULAR_EXPRESSION
        "SKIP.*platform cannot initialize the requested RHI context"
)
add_test(NAME terminal-glyph-batch-vulkan-rhi
    COMMAND test-terminal-glyph-batch-rhi)
set_tests_properties(terminal-glyph-batch-vulkan-rhi PROPERTIES
    TIMEOUT 20
    ENVIRONMENT
        "GHOSTTY_QT_GLYPH_BATCH_TEST_BACKEND=vulkan;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=vulkan;QT_SCALE_FACTOR=1"
    SKIP_REGULAR_EXPRESSION
        "SKIP.*platform cannot initialize the requested RHI context"
)

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

add_executable(pty-io-probe
    pty_io_probe.cpp
)
target_compile_options(pty-io-probe PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(environment-probe
    environment_probe.cpp
)
target_compile_options(environment-probe PRIVATE
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
    GHOSTTY_QT_TEST_FONT_PATH="${GHOSTTY_SOURCE_DIR}/pkg/freetype/res/FiraCode-Regular.ttf"
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    target_compile_definitions(test-ghostty-cli-delegation PRIVATE
        GHOSTTY_QT_TEST_REAL_HELPER="$<TARGET_FILE:ghostty-qt-config-helper>"
        GHOSTTY_QT_TEST_THEMES_PATH="${GHOSTTY_QT_THEMES_STAGE_DIR}"
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

qt_add_executable(test-ghostty-application-ipc
    test_ghostty_application_ipc.cpp
    private_session_bus.h
)
target_include_directories(test-ghostty-application-ipc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-ghostty-application-ipc PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    ghostty-qt-application-ipc
)
add_test(NAME ghostty-application-ipc
    COMMAND test-ghostty-application-ipc)
set_tests_properties(ghostty-application-ipc PROPERTIES TIMEOUT 25)

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

qt_add_executable(test-desktop-notification-service
    test_desktop_notification_service.cpp
)
target_include_directories(test-desktop-notification-service PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-desktop-notification-service PRIVATE
    GHOSTTY_QT_APPLICATION_ID="${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
)
target_link_libraries(test-desktop-notification-service PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    ghostty-qt-desktop-notification
)
add_test(NAME desktop-notification-service
    COMMAND test-desktop-notification-service)
set_tests_properties(desktop-notification-service PROPERTIES TIMEOUT 10)

qt_add_executable(test-application-lifetime
    test_application_lifetime.cpp
)
target_include_directories(test-application-lifetime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-application-lifetime PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-application-lifetime
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
)
target_include_directories(test-single-instance-activation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-single-instance-activation PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    ghostty-qt-single-instance-activation
)
add_test(NAME single-instance-activation
    COMMAND test-single-instance-activation)
set_tests_properties(single-instance-activation PROPERTIES TIMEOUT 25)

qt_add_executable(test-linux-cgroup
    test_linux_cgroup.cpp
    private_session_bus.h
)
target_include_directories(test-linux-cgroup PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-linux-cgroup PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    ghostty-qt-linux-cgroup
)
add_test(NAME linux-cgroup COMMAND test-linux-cgroup)
set_tests_properties(linux-cgroup PROPERTIES TIMEOUT 15)

ghostty_qt_add_internal_library(ghostty-qt-render-test-support
    ../src/terminal_inspector_event_model.cpp
    ../src/terminal_inspector_event_model.h
    ../src/terminal_inspector_model.cpp
    ../src/terminal_inspector_model.h
    ../src/terminal_pane.cpp
    ../src/terminal_pane.h
    ../src/terminal_pane_renderer.cpp
    ../src/terminal_pane_renderer.h
    ../src/terminal_pane_render_probe_p.h
    ../src/terminal_workspace.cpp
    ../src/terminal_workspace.h
)
target_compile_definitions(ghostty-qt-render-test-support PUBLIC
    GHOSTTY_QT_RENDER_TEST_PROBE=1
)
target_link_libraries(ghostty-qt-render-test-support PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    ghostty-qt-action-pipeline
    ghostty-qt-key-identity
    ghostty-qt-key-event-context
    ghostty-qt-keyboard-layout
    ghostty-qt-launch-options
    ghostty-qt-modifier-remap
    ghostty-qt-tab-list-model
    ghostty-qt-terminal-bell
    ghostty-qt-terminal-controller
    ghostty-qt-terminal-foundation
    ghostty-qt-terminal-rendering
)

qt_add_executable(test-application-controller
    test_application_controller.cpp
)
target_include_directories(test-application-controller PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_compile_definitions(test-application-controller PRIVATE
    GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE="$<TARGET_FILE:pty-geometry-probe>"
)
target_link_libraries(test-application-controller PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    ghostty-qt-application-controller
    ghostty-qt-quick-terminal
    ghostty-qt-render-test-support
    ghostty-qt-window-ui
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
)
target_include_directories(test-initial-session-coordinator PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-initial-session-coordinator PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-terminal-foundation
)
add_test(NAME initial-session-coordinator
    COMMAND test-initial-session-coordinator)
set_tests_properties(initial-session-coordinator PROPERTIES TIMEOUT 10)

qt_add_executable(test-terminal-types
    test_terminal_types.cpp
    ../src/terminal_actions.h
    ../src/terminal_clipboard.h
    ../src/terminal_clipboard_codepoint_map.h
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
    ghostty-qt-terminal-foundation
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

qt_add_executable(test-terminal-inspector-events
    test_terminal_inspector_events.cpp
    ../src/terminal_inspector_event_model.cpp
    ../src/terminal_inspector_event_model.h
)
target_include_directories(test-terminal-inspector-events PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-inspector-events PRIVATE
    Qt6::Core
    Qt6::Test
)
add_test(NAME terminal-inspector-events
    COMMAND test-terminal-inspector-events)
set_tests_properties(terminal-inspector-events PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-smoke
    test_ghostty_smoke.cpp
)
target_link_libraries(test-ghostty-smoke PRIVATE Qt6::Core Qt6::Test ghostty-vt-static)
add_test(NAME ghostty-smoke COMMAND test-ghostty-smoke)

qt_add_executable(test-session-worker
    test_session_worker.cpp
)
target_include_directories(test-session-worker PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_compile_definitions(test-session-worker PRIVATE
    GHOSTTY_QT_VERSION="${PROJECT_VERSION}"
    GHOSTTY_QT_TEST_ENVIRONMENT_PROBE="$<TARGET_FILE:environment-probe>"
    GHOSTTY_QT_TEST_PTY_GEOMETRY_PROBE="$<TARGET_FILE:pty-geometry-probe>"
    GHOSTTY_QT_TEST_PTY_IO_PROBE="$<TARGET_FILE:pty-io-probe>"
    GHOSTTY_QT_TEST_SHELL_INTEGRATION_ENABLED=$<BOOL:${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}>
)
target_link_libraries(test-session-worker PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-session-worker
)
add_dependencies(test-session-worker
    environment-probe
    ghostty-qt-terminfo
    pty-geometry-probe
    pty-io-probe
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    add_dependencies(test-session-worker
        ghostty-qt-config-helper
    )
endif()
add_test(NAME session-worker COMMAND test-session-worker)

qt_add_executable(test-terminal-pane
    test_terminal_pane.cpp
    ../src/terminal_pane_render_probe_p.h
)
target_include_directories(test-terminal-pane PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(test-terminal-pane PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Test
    ghostty-qt-render-test-support
)
add_dependencies(test-terminal-pane ghostty-qt-terminfo)
add_test(NAME terminal-pane-render COMMAND test-terminal-pane)
set_tests_properties(terminal-pane-render PROPERTIES
    TIMEOUT 75
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_SCALE_FACTOR=1"
)
add_test(
    NAME terminal-custom-shader-linear-wayland-rhi
    COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/run_wayland_rhi_test.sh"
        "$<TARGET_FILE:test-terminal-custom-shader-rhi>"
        retainedLinearModeUsesFloatTargetsAndReloads
)
set_tests_properties(terminal-custom-shader-linear-wayland-rhi PROPERTIES
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT
        "LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=wayland;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
)
add_test(
    NAME terminal-retained-rect-batch-wayland-rhi
    COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/run_wayland_rhi_test.sh"
        "$<TARGET_FILE:test-terminal-custom-shader-rhi>"
        retainedRectBatchRecoversAfterEmptyFrames
)
set_tests_properties(terminal-retained-rect-batch-wayland-rhi PROPERTIES
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT
        "LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=wayland;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
)
add_test(
    NAME terminal-pane-kitty-wayland-rhi
    COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/run_wayland_rhi_test.sh"
        "$<TARGET_FILE:test-terminal-pane>"
        rendersAndRetainsKittyGraphics
)
set_tests_properties(terminal-pane-kitty-wayland-rhi PROPERTIES
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT
        "GHOSTTY_QT_EXPECT_RHI=1;GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=wayland;QT_QUICK_BACKEND=rhi;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
)
add_test(
    NAME terminal-pane-glyph-batch-wayland-rhi
    COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/run_wayland_rhi_test.sh"
        "$<TARGET_FILE:test-terminal-pane>"
        retainsGlyphAtlasAndLazilyCreatesNativeTextRows
)
set_tests_properties(terminal-pane-glyph-batch-wayland-rhi PROPERTIES
    TIMEOUT 30
    SKIP_RETURN_CODE 77
    ENVIRONMENT
        "GHOSTTY_QT_DISABLE_GLYPH_BATCH=0;GHOSTTY_QT_EXPECT_RHI=1;GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};QT_QPA_PLATFORM=wayland;QT_QUICK_BACKEND=rhi;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
)
add_test(
    NAME terminal-pane-retained-solids-rhi
    COMMAND
        test-terminal-pane
        retainsMainTextRowsAcrossIncrementalUpdates
        invalidatesOnlyRowsAffectedByGlobalColors
        invalidatesHoveredExemptGlyphForContrastChanges
)
set_tests_properties(terminal-pane-retained-solids-rhi PROPERTIES
    TIMEOUT 30
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};LIBGL_ALWAYS_SOFTWARE=1;QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=opengl;QT_SCALE_FACTOR=1"
)

if(GHOSTTY_QT_BUILD_RENDER_BENCHMARKS)
    if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
        qt_add_executable(bench-ghostty-shell-integration
            bench_ghostty_shell_integration.cpp
        )
        target_include_directories(
            bench-ghostty-shell-integration PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../src"
        )
        target_compile_definitions(bench-ghostty-shell-integration PRIVATE
            GHOSTTY_QT_BENCH_SHELL_HELPER="$<TARGET_FILE:ghostty-qt-config-helper>"
            GHOSTTY_QT_BENCH_SHELL_RESOURCES="${CMAKE_BINARY_DIR}"
        )
        target_link_libraries(bench-ghostty-shell-integration PRIVATE
            Qt6::Core
            ghostty-qt-shell-integration
        )
        add_dependencies(bench-ghostty-shell-integration
            ghostty-qt-config-helper)
    endif()

    qt_add_executable(bench-terminal-custom-shader-compiler
        bench_terminal_custom_shader_compiler.cpp
    )
    target_include_directories(
        bench-terminal-custom-shader-compiler PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(bench-terminal-custom-shader-compiler PRIVATE
        Qt6::Core
        ghostty-qt-terminal-rendering
    )

    qt_add_executable(bench-terminal-custom-shader-rhi
        bench_terminal_custom_shader_rhi.cpp
        renderdoc_capture.cpp
        renderdoc_capture.h
    )
    target_include_directories(
        bench-terminal-custom-shader-rhi PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(bench-terminal-custom-shader-rhi PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Qml
        Qt6::Quick
        ghostty-qt-terminal-rendering
        ${CMAKE_DL_LIBS}
    )

    qt_add_executable(bench-terminal-pane-renderer
        bench_terminal_pane_renderer.cpp
        renderdoc_capture.cpp
        renderdoc_capture.h
        ../src/terminal_pane_render_probe_p.h
    )
    target_include_directories(bench-terminal-pane-renderer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(bench-terminal-pane-renderer PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::GuiPrivate
        Qt6::Qml
        Qt6::Quick
        Qt6::Test
        ghostty-qt-render-test-support
        ${CMAKE_DL_LIBS}
    )
    add_dependencies(bench-terminal-pane-renderer ghostty-qt-terminfo)

    qt_add_executable(bench-terminal-kitty-graphics
        bench_terminal_kitty_graphics.cpp
    )
    target_include_directories(bench-terminal-kitty-graphics PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(bench-terminal-kitty-graphics PRIVATE
        Qt6::Core
        Qt6::Gui
        ghostty-qt-vt-adapter
    )

    qt_add_executable(bench-terminal-frame-materialization
        bench_terminal_frame_materialization.cpp
    )
    target_include_directories(bench-terminal-frame-materialization PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(bench-terminal-frame-materialization PRIVATE
        Qt6::Core
        Qt6::Gui
        ghostty-qt-vt-adapter
    )

    qt_add_executable(bench-terminal-search
        bench_terminal_search.cpp
    )
    target_include_directories(bench-terminal-search PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_compile_definitions(bench-terminal-search PRIVATE
        GHOSTTY_QT_BENCH_TERMINFO="${GHOSTTY_QT_TERMINFO_DIR}"
    )
    target_link_libraries(bench-terminal-search PRIVATE
        Qt6::Core
        Qt6::Gui
        ghostty-qt-session-worker
    )
    add_dependencies(bench-terminal-search ghostty-qt-terminfo)

    qt_add_executable(bench-terminal-session-io
        bench_terminal_session_io.cpp
    )
    target_include_directories(
        bench-terminal-session-io PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_compile_definitions(bench-terminal-session-io PRIVATE
        GHOSTTY_QT_BENCH_TERMINFO="${GHOSTTY_QT_TERMINFO_DIR}"
    )
    target_link_libraries(bench-terminal-session-io PRIVATE
        Qt6::Core
        Qt6::Gui
        ghostty-qt-session-worker
    )
    add_dependencies(bench-terminal-session-io ghostty-qt-terminfo)

    qt_add_executable(bench-terminal-backdrop
        bench_terminal_backdrop.cpp
    )
    target_include_directories(bench-terminal-backdrop PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    )
    target_link_libraries(bench-terminal-backdrop PRIVATE
        Qt6::Core
        Qt6::Gui
        ghostty-qt-terminal-rendering
    )
endif()

qt_add_executable(test-terminal-workspace
    test_terminal_workspace.cpp
)
target_include_directories(test-terminal-workspace PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-terminal-workspace PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    ghostty-qt-application-keybindings
    ghostty-qt-render-test-support
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
    ../src/workspace_action.h
    ../src/workspace_ids.h
)
target_include_directories(test-workspace-foundation PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-workspace-foundation PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-tab-list-model
)
add_test(NAME workspace-foundation COMMAND test-workspace-foundation)
set_tests_properties(workspace-foundation PROPERTIES TIMEOUT 10)

qt_add_executable(test-tab-list-model
    test_tab_list_model.cpp
    ../src/workspace_ids.h
)
target_include_directories(test-tab-list-model PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-tab-list-model PRIVATE
    Qt6::Core
    Qt6::Test
    ghostty-qt-tab-list-model
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
)
target_include_directories(test-ghostty-global-shortcut-portal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-ghostty-global-shortcut-portal PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Test
    PkgConfig::XKBCommon
    ghostty-qt-global-shortcut-portal
)
add_test(NAME ghostty-global-shortcut-portal
    COMMAND test-ghostty-global-shortcut-portal)
set_tests_properties(ghostty-global-shortcut-portal PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-config-service
    test_ghostty_config_service.cpp
    ghostty_config_export_fixture.h
    ghostty_config_snapshot_fixture.h
    ../src/ghostty_config_loader.h
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
    ghostty-qt-config-runtime
)
add_test(NAME ghostty-config-service COMMAND test-ghostty-config-service)
set_tests_properties(ghostty-config-service PROPERTIES TIMEOUT 10)

qt_add_executable(test-ghostty-config-edit
    test_ghostty_config_edit.cpp
)
target_include_directories(test-ghostty-config-edit PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(test-ghostty-config-edit PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Test
    ghostty-qt-config-runtime
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
    ghostty-qt-config-runtime
)
add_dependencies(test-ghostty-config-process-loader
    fake-ghostty-config-helper)
add_test(NAME ghostty-config-process-loader
    COMMAND test-ghostty-config-process-loader)
set_tests_properties(ghostty-config-process-loader PROPERTIES TIMEOUT 30)

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
set_tests_properties(ghostty-config-export PROPERTIES TIMEOUT 30)

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
    NAME renderer-qualification-harness
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer_qualification.py"
)
set_tests_properties(renderer-qualification-harness PROPERTIES TIMEOUT 10)

add_test(
    NAME renderer-qualification-comparison
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/test_renderer_qualification_comparison.py"
)
set_tests_properties(renderer-qualification-comparison PROPERTIES TIMEOUT 10)

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
add_test(
    NAME application-lifecycle-dash-e
    COMMAND $<TARGET_FILE:ghostty-qt> -e /bin/true
)
set(_ghostty_qt_lifecycle_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-lifecycle-config")
file(MAKE_DIRECTORY "${_ghostty_qt_lifecycle_config_home}")
set_tests_properties(application-lifecycle application-lifecycle-dash-e
    PROPERTIES
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
    NAME application-context-menu-position
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_context_menu_position_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-context-menu-position-config")
file(MAKE_DIRECTORY "${_ghostty_qt_context_menu_position_config_home}")
set_tests_properties(application-context-menu-position PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_CONTEXT_MENU_POSITION=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_context_menu_position_config_home}"
    FAIL_REGULAR_EXPRESSION
        "Binding loop detected;QQmlApplicationEngine failed"
)

add_test(
    NAME application-context-menu-action
    COMMAND $<TARGET_FILE:ghostty-qt> --hold -- /bin/true
)
set(_ghostty_qt_context_menu_action_config_home
    "${CMAKE_CURRENT_BINARY_DIR}/application-context-menu-action-config")
file(MAKE_DIRECTORY "${_ghostty_qt_context_menu_action_config_home}")
set_tests_properties(application-context-menu-action PROPERTIES
    TIMEOUT 5
    ENVIRONMENT
        "GHOSTTY_QT_ALLOW_NON_WAYLAND=1;GHOSTTY_QT_TEST_CONTEXT_MENU_ACTION=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FATAL_WARNINGS=1;XDG_CONFIG_HOME=${_ghostty_qt_context_menu_action_config_home}"
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

# Qt may otherwise route diagnostics to journald when CTest captures a pipe.
# Keep QT_FATAL_WARNINGS failures visible in the test log instead of reporting
# only an opaque subprocess abort.
set(_ghostty_qt_headless_application_tests
    application-lifecycle
    application-lifecycle-dash-e
    application-close-dialog
    application-tab-title-prompt
    application-surface-title-prompt
    application-context-menu-position
    application-context-menu-action
    application-fullscreen-action
    application-maximize-action
    application-window-decoration-action
    application-tab-bar-visibility
    application-tabs-location
)
if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    list(APPEND _ghostty_qt_headless_application_tests
        application-lifetime-resident
        application-lifetime-explicit-quit
        application-initial-window-state
        application-initial-window-size
    )
endif()
set_property(TEST ${_ghostty_qt_headless_application_tests} APPEND PROPERTY
    ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1")

set_tests_properties(launch-options ghostty-smoke session-worker PROPERTIES
    TIMEOUT 20
)
set_tests_properties(session-worker PROPERTIES
    TIMEOUT 30
    ENVIRONMENT
        "GHOSTTY_QT_TERMINFO=${GHOSTTY_QT_TERMINFO_DIR};GHOSTTY_QT_SHELL_INTEGRATION_RESOURCES=${CMAKE_BINARY_DIR}"
)

add_test(
    NAME desktop-integration-install
    COMMAND "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DSTAGE_DIR=${CMAKE_SOURCE_DIR}/tmp/desktop-integration-install-${CMAKE_BUILD_TYPE}-${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}"
        "-DREPOSITORY_TMP_DIR=${CMAKE_SOURCE_DIR}/tmp"
        "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
        "-DINSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
        "-DAPPLICATION_ID=${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
        "-DEXPECT_CONFIG_HELPER=${GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG}"
        "-DSHELL_CACHE_PROBE=${_ghostty_qt_shell_integration_cache_probe}"
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
            "-DINSTALL_SHELL_INTEGRATION_DIR=${GHOSTTY_QT_INSTALL_SHELL_INTEGRATION_DIR}"
            "-DINSTALL_THEMES_DIR=${GHOSTTY_QT_INSTALL_THEMES_DIR}"
            "-DSHELL_INTEGRATION_VALIDATE_SCRIPT=${GHOSTTY_QT_SHELL_INTEGRATION_VALIDATE_SCRIPT}"
            "-DPROBE=$<TARGET_FILE:test-terminfo-path-probe>"
            "-DCONFIG_HELPER_NAME=${_ghostty_qt_relocation_config_helper_name}"
            "-DCONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/RunTerminfoRelocationTest.cmake"
    )
    set_tests_properties(terminfo-relocatable-install PROPERTIES TIMEOUT 30)
endif()
