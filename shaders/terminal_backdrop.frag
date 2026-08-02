#version 440

layout(location = 0) in vec2 panePosition;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D straightRgba;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float imageOpacity;
    float repeatImage;
    float linearBlending;
    vec4 background;
    vec4 destination;
} uniforms;

vec3 srgbToLinear(vec3 component)
{
    bvec3 low = lessThanEqual(component, vec3(0.04045));
    vec3 high = pow((component + 0.055) / 1.055, vec3(2.4));
    return mix(high, component / 12.92, low);
}

void main()
{
    vec2 coordinate =
        (panePosition - uniforms.destination.xy)
        / uniforms.destination.zw;
    bool outside =
        any(lessThan(coordinate, vec2(0.0)))
        || any(greaterThan(coordinate, vec2(1.0)));
    bool repeating = uniforms.repeatImage > 0.5;
    if (repeating) {
        coordinate =
            mod(mod(coordinate, vec2(1.0)) + vec2(1.0), vec2(1.0));
    }

    vec4 image = vec4(0.0);
    if (repeating || !outside) {
        image = texture(straightRgba, coordinate);
        if (uniforms.linearBlending > 0.5) {
            image.rgb = srgbToLinear(image.rgb);
        }
        image.rgb *= image.a;
    }

    float relativeOpacity = uniforms.background.a > 0.0
        ? min(uniforms.imageOpacity, 1.0 / uniforms.background.a)
        : uniforms.imageOpacity;
    image *= relativeOpacity;
    vec3 background = uniforms.background.rgb;
    if (uniforms.linearBlending > 0.5) {
        background = srgbToLinear(background);
    }
    image += max(vec4(0.0), vec4(background, 1.0) * (1.0 - image.a));
    image *= uniforms.background.a;
    fragmentColor = image * uniforms.qt_Opacity;
}
