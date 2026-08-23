# Keyboard identity, keybinding compilation, and input normalization.

ghostty_qt_add_internal_library(ghostty-qt-key-identity
    src/input/ghostty_key_identity.cpp
    src/input/ghostty_key_identity.h
)
target_link_libraries(
    ghostty-qt-key-identity
    PUBLIC Qt6::Core Qt6::Gui ghostty-vt-static
    PRIVATE PkgConfig::XKBCommon
)

ghostty_qt_add_internal_library(ghostty-qt-key-event-context
    src/input/key_event_context.cpp
    src/input/key_event_context.h
)
target_link_libraries(ghostty-qt-key-event-context PUBLIC Qt6::Gui)

ghostty_qt_add_internal_library(ghostty-qt-keyboard-layout
    src/input/keyboard_layout.cpp
    src/input/keyboard_layout.h
    src/support/unique_file_descriptor.h
)
target_link_libraries(
    ghostty-qt-keyboard-layout
    PUBLIC Qt6::Core Qt6::Gui
    PRIVATE PkgConfig::WaylandClient PkgConfig::XKBCommon
)

ghostty_qt_add_internal_library(ghostty-qt-modifier-remap
    src/input/modifier_remap.cpp
    src/input/modifier_remap.h
    src/input/modifier_remap_types.h
)
target_link_libraries(ghostty-qt-modifier-remap PUBLIC Qt6::Core Qt6::Gui)
target_link_libraries(ghostty-qt-modifier-remap PRIVATE ghostty-qt-key-identity)

add_library(
    ghostty-qt-action-pipeline
    STATIC
    src/input/ghostty_action_catalog.cpp
    src/input/ghostty_action_catalog.h
    src/input/ghostty_keybind_set.cpp
    src/input/ghostty_keybind_set.h
    src/input/zig_string_escape.cpp
    src/input/zig_string_escape.h
)
target_include_directories(
    ghostty-qt-action-pipeline
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-action-pipeline PUBLIC Qt6::Core Qt6::Gui)
target_link_libraries(
    ghostty-qt-action-pipeline
    PRIVATE ghostty-qt-key-identity
)
target_compile_options(
    ghostty-qt-action-pipeline
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-application-keybindings
    src/input/ghostty_application_keybindings.cpp
    src/input/ghostty_application_keybindings.h
)
target_link_libraries(
    ghostty-qt-application-keybindings
    PUBLIC
        Qt6::Core
        Qt6::Gui
        ghostty-qt-action-pipeline
        ghostty-qt-key-event-context
        ghostty-qt-keyboard-layout
        ghostty-qt-launch-options
        ghostty-qt-modifier-remap
    PRIVATE Qt6::DBus Qt6::Qml Qt6::Quick ghostty-qt-global-shortcut-portal
)
