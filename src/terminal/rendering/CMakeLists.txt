# Scene-graph, glyph, image, backdrop, and custom-shader rendering.

add_library(
    ghostty-qt-terminal-rendering
    STATIC
    src/terminal/rendering/terminal_alpha_blending.cpp
    src/terminal/rendering/terminal_alpha_blending.h
    src/terminal/rendering/terminal_custom_shader_compiler.cpp
    src/terminal/rendering/terminal_custom_shader_compiler.h
    src/terminal/model/terminal_custom_shader_options.h
    src/terminal/rendering/terminal_custom_shader_pipeline.cpp
    src/terminal/rendering/terminal_custom_shader_pipeline.h
    src/terminal/rendering/terminal_custom_shader_qsg.cpp
    src/terminal/rendering/terminal_custom_shader_qsg.h
    src/terminal/rendering/terminal_backdrop.cpp
    src/terminal/rendering/terminal_backdrop.h
    src/terminal/rendering/terminal_backdrop_p.h
    src/terminal/rendering/terminal_backdrop_qsg.cpp
    src/terminal/rendering/terminal_backdrop_qsg.h
    src/terminal/rendering/terminal_kitty_graphics.cpp
    src/terminal/rendering/terminal_kitty_graphics.h
    src/terminal/rendering/terminal_kitty_graphics_qsg.cpp
    src/terminal/rendering/terminal_kitty_graphics_qsg.h
    src/terminal/rendering/terminal_glyph_atlas.cpp
    src/terminal/rendering/terminal_glyph_atlas.h
    src/terminal/rendering/terminal_glyph_batch.cpp
    src/terminal/rendering/terminal_glyph_batch.h
    src/terminal/rendering/terminal_rect_batch.cpp
    src/terminal/rendering/terminal_rect_batch.h
    src/terminal/rendering/terminal_straight_rgba_texture.cpp
    src/terminal/rendering/terminal_straight_rgba_texture_p.h
)
target_include_directories(
    ghostty-qt-terminal-rendering
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
    ghostty-qt-terminal-rendering
    PUBLIC Qt6::Core Qt6::Gui Qt6::Quick ghostty-qt-config-export
    PRIVATE Qt6::GuiPrivate Qt6::ShaderToolsPrivate
)
target_compile_options(
    ghostty-qt-terminal-rendering
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
qt_add_shaders(
    ghostty-qt-terminal-rendering
    ghostty_qt_terminal_backdrop_shaders

    BATCHABLE

    PREFIX
        "/ghostty-qt/shaders"

    FILES
        shaders/terminal_backdrop.vert
        shaders/terminal_backdrop.frag

    OUTPUTS
        terminal_backdrop.vert.qsb
        terminal_backdrop.frag.qsb
)
qt_add_shaders(
    ghostty-qt-terminal-rendering
    ghostty_qt_terminal_custom_shader_shaders

    BATCHABLE
    NOHLSL
    NOMSL
    GLSL
        "300 es,330,430"

    PREFIX
        "/ghostty-qt/shaders"

    FILES
        shaders/terminal_custom_shader.vert
        shaders/terminal_alpha_encode.frag

    OUTPUTS
        terminal_custom_shader.vert.qsb
        terminal_alpha_encode.frag.qsb
)
qt_add_shaders(
    ghostty-qt-terminal-rendering
    ghostty_qt_terminal_kitty_shaders

    BATCHABLE

    PREFIX
        "/ghostty-qt/shaders"

    FILES
        shaders/terminal_kitty.vert
        shaders/terminal_kitty.frag

    OUTPUTS
        terminal_kitty.vert.qsb
        terminal_kitty.frag.qsb
)
qt_add_shaders(
    ghostty-qt-terminal-rendering
    ghostty_qt_terminal_glyph_shaders

    BATCHABLE

    PREFIX
        "/ghostty-qt/shaders"

    FILES
        shaders/terminal_glyph.vert
        shaders/terminal_glyph.frag

    OUTPUTS
        terminal_glyph.vert.qsb
        terminal_glyph.frag.qsb
)

ghostty_qt_add_internal_library(ghostty-qt-renderer-graphics-library-manifest
    src/terminal/rendering/renderer_graphics_library_manifest.cpp
    src/terminal/rendering/renderer_graphics_library_manifest.h
)
target_link_libraries(
    ghostty-qt-renderer-graphics-library-manifest
    PUBLIC Qt6::Core
)
