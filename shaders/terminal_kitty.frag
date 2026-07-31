#version 440

layout(location = 0) in vec2 imageCoordinate;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D straightRgba;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} uniforms;

void main()
{
    vec4 image = texture(straightRgba, imageCoordinate);
    image.rgb *= image.a;
    fragmentColor = image * uniforms.qt_Opacity;
}
