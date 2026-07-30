#version 440

layout(location = 0) in vec4 vertex;
layout(location = 1) in vec2 textureCoordinate;
layout(location = 0) out vec2 qt_TexCoord0;

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
};

out gl_PerVertex {
    vec4 gl_Position;
};

void main()
{
    gl_Position = qt_Matrix * vertex;
    qt_TexCoord0 = textureCoordinate;
}
