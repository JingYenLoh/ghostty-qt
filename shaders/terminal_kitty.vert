#version 440

layout(location = 0) in vec4 vertex;
layout(location = 1) in vec2 textureCoordinate;

layout(location = 0) out vec2 imageCoordinate;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} uniforms;

out gl_PerVertex {
    vec4 gl_Position;
};

void main()
{
    gl_Position = uniforms.qt_Matrix * vertex;
    imageCoordinate = textureCoordinate;
}
