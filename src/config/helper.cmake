# Configuration parser helper executable.

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    add_executable(
        ghostty-qt-config-helper
        src/config/ghostty_config_cli_main.cpp
    )
    target_link_libraries(
        ghostty-qt-config-helper
        PRIVATE ghostty-config-internal ghostty-qt-cli-delegation
    )
    target_compile_options(
        ghostty-qt-config-helper
        PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
    )
    target_compile_definitions(
        ghostty-qt-config-helper
        PRIVATE
            GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR="${GHOSTTY_QT_INSTALL_RESOURCES_RELATIVE_DIR}"
    )
    set_target_properties(
        ghostty-qt-config-helper
        PROPERTIES
            BUILD_RPATH "${GHOSTTY_QT_CONFIG_PREFIX}/lib"
            # Keep the build-tree RUNPATH exact rather than letting CMake append an
            # empty padding component (which the dynamic loader treats as cwd).
            # The install step rewrites this absolute path to the relative one.
            INSTALL_RPATH "${GHOSTTY_QT_CONFIG_PREFIX}/lib"
            BUILD_WITH_INSTALL_RPATH TRUE
    )

    target_compile_definitions(
        ghostty-qt
        PRIVATE GHOSTTY_QT_CONFIG_HELPER_NAME="ghostty-qt-config-helper"
    )
    add_dependencies(ghostty-qt ghostty-qt-config-helper)
endif()
