#version 440
// The blend is dst * (1 - alpha): coverage times opacity punches through.
layout(location = 0) in vec2 vPos;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;
    float radius;
};

void main() {
    vec2 q = abs(vPos - size * 0.5) - (size * 0.5 - vec2(radius));
    float distance = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
    fragColor = vec4(0.0, 0.0, 0.0, clamp(0.5 - distance, 0.0, 1.0) * qt_Opacity);
}
