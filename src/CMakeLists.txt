function(ghostty_qt_add_internal_library target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(
        ${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/support/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/config/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/input/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/desktop/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/session/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/terminal/core/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/terminal/rendering/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/terminal/adapter/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/workspace/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/app/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/testing/CMakeLists.txt")
