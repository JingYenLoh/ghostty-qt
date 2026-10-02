# Configuration parser helper executable.

if(GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG)
    # The embedded Linux runtime now references EGL even without a GTK app.
    find_package(OpenGL REQUIRED COMPONENTS EGL)
    add_executable(
        ghostty-qt-config-helper
        src/config/ghostty_config_cli_main.cpp
        "${GHOSTTY_SOURCE_DIR}/vendor/glad/src/glad_egl.c"
    )
    target_link_libraries(
        ghostty-qt-config-helper
        PRIVATE ghostty-config-internal ghostty-qt-cli-delegation OpenGL::EGL
    )
    target_include_directories(
        ghostty-qt-config-helper
        PRIVATE "${GHOSTTY_SOURCE_DIR}/vendor/glad/include"
    )
    # Upstream's embedded library leaves its EGL loader to the host executable.
    target_link_options(
        ghostty-qt-config-helper
        PRIVATE "LINKER:--export-dynamic-symbol=gladLoadEGL"
    )
    target_compile_options(
        ghostty-qt-config-helper
        PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall;-Wextra;-Wpedantic>
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
