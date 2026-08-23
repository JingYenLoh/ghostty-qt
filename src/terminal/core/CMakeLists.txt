# Terminal-domain behavior and shared foundation implementation.

add_library(
    ghostty-qt-terminal-bell
    STATIC
    src/terminal/core/terminal_bell.cpp
    src/terminal/core/terminal_bell.h
)
target_include_directories(
    ghostty-qt-terminal-bell
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    ghostty-qt-terminal-bell
    PUBLIC Qt6::Core Qt6::Gui
    PRIVATE Qt6::Multimedia Qt6::Widgets
)
target_compile_options(
    ghostty-qt-terminal-bell
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-terminal-foundation
    src/session/initial_session_coordinator.cpp
    src/session/initial_session_coordinator.h
    src/terminal/core/terminal_cell_metrics.cpp
    src/terminal/core/terminal_cell_metrics.h
    src/terminal/core/terminal_clipboard.cpp
    src/terminal/core/terminal_clipboard.h
    src/terminal/model/terminal_clipboard_codepoint_map.h
    src/terminal/core/terminal_drop.cpp
    src/terminal/core/terminal_drop.h
    src/terminal/core/terminal_font_resolver.cpp
    src/terminal/core/terminal_font_resolver.h
    src/terminal/core/terminal_geometry.cpp
    src/terminal/core/terminal_geometry.h
    src/terminal/core/terminal_glyph_plan.cpp
    src/terminal/core/terminal_glyph_plan.h
    src/terminal/model/terminal_path.h
    src/terminal/model/terminal_session_options.h
    src/terminal/core/terminal_text_grid_fit.cpp
    src/terminal/core/terminal_text_grid_fit.h
    src/terminal/core/terminal_text_runs.cpp
    src/terminal/core/terminal_text_runs.h
    src/terminal/model/terminal_typography.h
    src/terminal/model/terminal_types.h
)
target_link_libraries(ghostty-qt-terminal-foundation PUBLIC Qt6::Core Qt6::Gui)
