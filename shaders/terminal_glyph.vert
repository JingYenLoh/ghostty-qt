#version 440

layout(location = 0) in vec2 vertexPosition;
layout(location = 1) in vec2 textureCoordinate;
layout(location = 2) in vec4 premultipliedColor;

layout(location = 0) out vec2 glyphCoordinate;
layout(location = 1) out vec4 glyphColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
} uniforms;

out gl_PerVertex {
    vec4 gl_Position;
};

void main()
{
    gl_Position = uniforms.qt_Matrix * vec4(vertexPosition, 0.0, 1.0);
    glyphCoordinate = textureCoordinate;
    glyphColor = premultipliedColor;
}
