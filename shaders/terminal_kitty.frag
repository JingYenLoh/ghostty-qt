#version 440

layout(location = 0) in vec2 imageCoordinate;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D straightRgba;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float linearBlending;
} uniforms;

vec3 srgbToLinear(vec3 component)
{
    bvec3 low = lessThanEqual(component, vec3(0.04045));
    vec3 high = pow((component + 0.055) / 1.055, vec3(2.4));
    return mix(high, component / 12.92, low);
}

void main()
{
    vec4 image = texture(straightRgba, imageCoordinate);
    if (uniforms.linearBlending > 0.5) {
        image.rgb = srgbToLinear(image.rgb);
    }
    image.rgb *= image.a;
    fragmentColor = image * uniforms.qt_Opacity;
}
