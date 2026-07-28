#version 440

layout(location = 0) in vec4 vertex;

layout(location = 0) out vec2 panePosition;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float imageOpacity;
    float repeatImage;
    float _padding;
    vec4 background;
    vec4 destination;
} uniforms;

out gl_PerVertex {
    vec4 gl_Position;
};

void main()
{
    gl_Position = uniforms.qt_Matrix * vertex;
    panePosition = vertex.xy;
}
