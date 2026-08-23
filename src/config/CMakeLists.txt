# Frontend and Ghostty configuration projections and services.

add_library(
    ghostty-qt-frontend-config
    STATIC
    src/config/frontend_config.cpp
    src/config/frontend_config.h
    src/config/frontend_config_service.cpp
    src/config/frontend_config_service.h
)
target_include_directories(
    ghostty-qt-frontend-config
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-frontend-config PUBLIC Qt6::Core Qt6::Gui)
target_link_libraries(
    ghostty-qt-frontend-config
    PRIVATE ghostty-qt-posix-regular-file
)
target_compile_options(
    ghostty-qt-frontend-config
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_library(
    ghostty-qt-config-export
    STATIC
    src/config/ghostty_config_export.cpp
    src/config/ghostty_config_export.h
    src/config/ghostty_config_values.h
    src/config/ghostty_keybind_config.h
    src/terminal/model/terminal_custom_shader_options.h
    src/terminal/model/terminal_command.h
    src/terminal/model/terminal_path.h
)
target_include_directories(
    ghostty-qt-config-export
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-config-export PUBLIC Qt6::Core Qt6::Gui)
target_compile_options(
    ghostty-qt-config-export
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

ghostty_qt_add_internal_library(ghostty-qt-config-runtime
    src/config/ghostty_config_edit.cpp
    src/config/ghostty_config_edit.h
    src/config/ghostty_config_loader.h
    src/config/ghostty_config_process_loader.cpp
    src/config/ghostty_config_process_loader.h
    src/config/ghostty_config_service.cpp
    src/config/ghostty_config_service.h
    src/config/ghostty_config_snapshot.h
)
target_link_libraries(
    ghostty-qt-config-runtime
    PUBLIC Qt6::Core Qt6::Gui ghostty-qt-config-export
)
target_link_libraries(
    ghostty-qt-config-runtime
    PRIVATE ghostty-qt-action-pipeline ghostty-qt-posix-regular-file
)
