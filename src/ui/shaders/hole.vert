#version 440
layout(location = 0) in vec4 position;
layout(location = 1) in vec2 texcoord;
layout(location = 0) out vec2 vPos;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
};

void main() {
    vPos = texcoord * size;
    gl_Position = qt_Matrix * position;
}
