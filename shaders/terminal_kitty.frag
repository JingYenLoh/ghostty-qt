#version 440

layout(location = 0) in vec2 imageCoordinate;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D straightRgb;
layout(binding = 2) uniform sampler2D straightAlpha;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} uniforms;

void main()
{
    vec4 image = vec4(texture(straightRgb, imageCoordinate).rgb,
                      texture(straightAlpha, imageCoordinate).r);
    image.rgb *= image.a;
    fragmentColor = image * uniforms.qt_Opacity;
}
