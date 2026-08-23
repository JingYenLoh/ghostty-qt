# Value boundary around the public libghostty-vt API.

add_library(
    ghostty-qt-vt-adapter
    STATIC
    src/terminal/adapter/ghostty_vt_adapter.cpp
    src/terminal/adapter/ghostty_vt_adapter.h
    src/terminal/model/terminal_desktop_notification.h
    src/terminal/inspector/terminal_inspector_snapshot.h
    src/terminal/adapter/terminal_kitty_image_materialization.cpp
    src/terminal/adapter/terminal_kitty_image_materialization.h
    src/terminal/model/terminal_progress_report.h
)
target_include_directories(
    ghostty-qt-vt-adapter
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    ghostty-qt-vt-adapter
    PUBLIC Qt6::Core Qt6::Gui
    PRIVATE ghostty-qt-key-identity ghostty-vt-static
)
target_compile_options(
    ghostty-qt-vt-adapter
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
