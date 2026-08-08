#version 440

layout(location = 0) in vec2 glyphCoordinate;
layout(location = 1) in vec4 glyphColor;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D glyphAtlas;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} uniforms;

void main()
{
    float coverage = texture(glyphAtlas, glyphCoordinate).a;
    fragmentColor = glyphColor * (coverage * uniforms.qt_Opacity);
}
