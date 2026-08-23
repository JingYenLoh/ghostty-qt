cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0205)
    cmake_policy(SET CMP0205 NEW)
endif()

project(ghostty-qt VERSION 0.1.0 LANGUAGES C CXX)

set(GHOSTTY_QT_APPLICATION_ID "io.github.JingYenLoh.ghostty_qt")
set(GHOSTTY_QT_EFFECTIVE_APPLICATION_ID
    "${GHOSTTY_QT_APPLICATION_ID}$<$<CONFIG:Debug>:.Debug>"
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ghostty-qt supports Linux only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

set(GHOSTTY_QT_UNINSTALL_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Uninstall.cmake.in"
    "${GHOSTTY_QT_UNINSTALL_SCRIPT}"
    @ONLY
)
add_custom_target(
    uninstall
    COMMAND "${CMAKE_COMMAND}" -P "${GHOSTTY_QT_UNINSTALL_SCRIPT}"
    COMMENT "Uninstall files recorded by the latest CMake install"
)

option(GHOSTTY_QT_BUILD_TESTS "Build ghostty-qt tests" ON)
option(
    GHOSTTY_QT_BUILD_RENDER_BENCHMARKS
    "Build opt-in terminal performance microbenchmarks"
    OFF
)
option(
    GHOSTTY_QT_ALLOW_UNPINNED_GHOSTTY
    "Allow a Ghostty checkout other than the pinned revision"
    OFF
)
option(
    GHOSTTY_QT_ENABLE_GHOSTTY_CONFIG
    "Build the pinned Ghostty application config parser"
    ON
)

set(GHOSTTY_QT_GHOSTTY_REVISION_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/GHOSTTY_REVISION"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GhosttyCheckout.cmake")

find_package(
    Qt6
    6.10
    REQUIRED
    COMPONENTS
        Core
        DBus
        Gui
        Multimedia
        Qml
        Quick
        QuickControls2
        ShaderTools
        Widgets
)
# The official Qt SDK exports these targets with their public modules, while
# distribution packages may split them into standalone CMake components.
if(NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6 6.10 REQUIRED COMPONENTS GuiPrivate)
endif()
if(NOT TARGET Qt6::ShaderToolsPrivate)
    find_package(Qt6 6.10 REQUIRED COMPONENTS ShaderToolsPrivate)
endif()
find_package(LayerShellQt 6.6.4 REQUIRED)
find_package(KF6WindowSystem QUIET)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XKBCommon REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
qt_standard_project_setup(REQUIRES 6.10)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GhosttyVt.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GhosttyResources.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GhosttyConfig.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/GhosttyLinkMatcher.cmake")

# libghostty-vt has public C struct fields named `emit`. Disabling Qt's
# convenience keywords avoids a preprocessor collision with that valid API.
add_compile_definitions(QT_NO_KEYWORDS)

# Keep the source target fragments in this directory scope so Qt preserves the
# existing root-relative QML and shader resource aliases.
include("${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallGhosttyQt.cmake")

if(GHOSTTY_QT_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 6.10 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
elseif(GHOSTTY_QT_BUILD_RENDER_BENCHMARKS)
    message(
        FATAL_ERROR
        "GHOSTTY_QT_BUILD_RENDER_BENCHMARKS requires GHOSTTY_QT_BUILD_TESTS"
    )
endif()
