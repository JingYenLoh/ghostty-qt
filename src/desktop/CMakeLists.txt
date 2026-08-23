# Linux desktop, Wayland, D-Bus, and window-system integration.

add_library(
    ghostty-qt-desktop-quick-controls-style
    STATIC
    src/desktop/desktop_quick_controls_style.cpp
    src/desktop/desktop_quick_controls_style.h
)
target_link_libraries(ghostty-qt-desktop-quick-controls-style PUBLIC Qt6::Core)
target_include_directories(
    ghostty-qt-desktop-quick-controls-style
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

ghostty_qt_add_internal_library(ghostty-qt-systemd-notify
    src/desktop/systemd_notify.cpp
    src/desktop/systemd_notify.h
    src/support/unique_file_descriptor.h
)
target_link_libraries(ghostty-qt-systemd-notify PUBLIC Qt6::Core)

ghostty_qt_add_internal_library(ghostty-qt-quick-terminal
    src/desktop/quick_terminal.cpp
    src/desktop/quick_terminal.h
    src/desktop/quick_terminal_surface.cpp
    src/desktop/quick_terminal_surface.h
)
target_link_libraries(
    ghostty-qt-quick-terminal
    PUBLIC Qt6::Core Qt6::Gui LayerShellQt::Interface
)

ghostty_qt_add_internal_library(ghostty-qt-window-effects
    src/desktop/window_blur_controller.cpp
    src/desktop/window_blur_controller.h
)
target_link_libraries(ghostty-qt-window-effects PUBLIC Qt6::Core Qt6::Gui)
if(TARGET KF6::WindowSystem)
    target_compile_definitions(
        ghostty-qt-window-effects
        PRIVATE GHOSTTY_QT_HAVE_KWINDOWEFFECTS=1
    )
    target_link_libraries(ghostty-qt-window-effects PRIVATE KF6::WindowSystem)
else()
    target_compile_definitions(
        ghostty-qt-window-effects
        PRIVATE GHOSTTY_QT_HAVE_KWINDOWEFFECTS=0
    )
endif()

add_library(
    ghostty-qt-desktop-activation
    STATIC
    src/desktop/desktop_activation.cpp
    src/desktop/desktop_activation.h
)
target_include_directories(
    ghostty-qt-desktop-activation
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    ghostty-qt-desktop-activation
    PUBLIC Qt6::Core
    PRIVATE Qt6::Gui
)
target_compile_options(
    ghostty-qt-desktop-activation
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(
    ghostty-qt-linux-cgroup
    STATIC
    src/desktop/linux_cgroup.cpp
    src/desktop/linux_cgroup.h
    src/desktop/linux_cgroup_config.h
)
target_include_directories(
    ghostty-qt-linux-cgroup
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-linux-cgroup PUBLIC Qt6::Core Qt6::DBus)
target_compile_options(
    ghostty-qt-linux-cgroup
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-global-shortcut-portal
    src/desktop/ghostty_global_shortcut_portal.cpp
    src/desktop/ghostty_global_shortcut_portal.h
)

ghostty_qt_add_internal_library(ghostty-qt-desktop-notification
    src/desktop/desktop_notification_service.cpp
    src/desktop/desktop_notification_service.h
    src/terminal/model/terminal_desktop_notification.h
    src/workspace/workspace_ids.h
)
target_compile_definitions(
    ghostty-qt-desktop-notification
    PRIVATE GHOSTTY_QT_APPLICATION_ID="${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
)
target_link_libraries(
    ghostty-qt-desktop-notification
    PUBLIC Qt6::Core Qt6::DBus
)
target_link_libraries(
    ghostty-qt-global-shortcut-portal
    PUBLIC Qt6::Core Qt6::DBus
    PRIVATE PkgConfig::XKBCommon
)

ghostty_qt_add_internal_library(ghostty-qt-single-instance-activation
    src/desktop/single_instance_activation.cpp
    src/desktop/single_instance_activation.h
)
target_compile_definitions(
    ghostty-qt-single-instance-activation
    PRIVATE GHOSTTY_QT_APPLICATION_ID="${GHOSTTY_QT_EFFECTIVE_APPLICATION_ID}"
)
target_link_libraries(
    ghostty-qt-single-instance-activation
    PUBLIC Qt6::Core Qt6::DBus ghostty-qt-desktop-activation
)
