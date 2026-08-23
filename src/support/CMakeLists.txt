# Low-level implementation support shared across runtime domains.

add_library(
    ghostty-qt-posix-regular-file
    STATIC
    src/support/posix_regular_file.cpp
    src/support/posix_regular_file.h
    src/support/unique_file_descriptor.h
)
target_include_directories(
    ghostty-qt-posix-regular-file
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ghostty-qt-posix-regular-file PUBLIC Qt6::Core)
target_compile_options(
    ghostty-qt-posix-regular-file
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
