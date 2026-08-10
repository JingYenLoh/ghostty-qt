#version 440

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragmentColor;

layout(binding = 1) uniform sampler2D iChannel0;

// This block intentionally mirrors every custom-shader uniform. Sharing the
// ABI lets the built-in color-space pass use both retained and legacy paths.
layout(std140, binding = 0) uniform GhosttyQtGlobals {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec3 iResolution;
    float iTime;
    float iTimeDelta;
    float iFrameRate;
    int iFrame;
    float iChannelTime[4];
    vec3 iChannelResolution[4];
    vec4 iMouse;
    vec4 iDate;
    float iSampleRate;
    vec4 iCurrentCursor;
    vec4 iPreviousCursor;
    vec4 iCurrentCursorColor;
    vec4 iPreviousCursorColor;
    int iCurrentCursorStyle;
    int iPreviousCursorStyle;
    int iCursorVisible;
    float iTimeCursorChange;
    float iTimeFocus;
    int iFocus;
    vec3 iPalette[256];
    vec3 iBackgroundColor;
    vec3 iForegroundColor;
    vec3 iCursorColor;
    vec3 iCursorText;
    vec3 iSelectionForegroundColor;
    vec3 iSelectionBackgroundColor;
    float _ghosttyQtPadding;
    vec4 iPaneTransition;
};

vec3 linearToSrgb(vec3 component)
{
    bvec3 low = lessThanEqual(component, vec3(0.0031308));
    vec3 high = 1.055 * pow(max(component, vec3(0.0)), vec3(1.0 / 2.4))
        - 0.055;
    return mix(high, 12.92 * component, low);
}

void main()
{
    vec4 linearPremultiplied = texture(iChannel0, qt_TexCoord0);
    if (linearPremultiplied.a > 0.0) {
        vec3 straight = linearPremultiplied.rgb / linearPremultiplied.a;
        linearPremultiplied.rgb =
            linearToSrgb(straight) * linearPremultiplied.a;
    }
    fragmentColor = linearPremultiplied * qt_Opacity;
}
