#version 440
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
};
layout(binding = 1) uniform sampler2D atlas;
void main() {
    // atlas is a single-channel (R) coverage; tint by the per-vertex colour, premultiplied.
    float a = texture(atlas, vUv).r;
    fragColor = vColor * (a * qt_Opacity);
}
