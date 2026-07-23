#version 440
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vIsColor;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
};
layout(binding = 1) uniform sampler2D atlas;
void main() {
    // Premultiplied RGBA atlas. Monochrome glyphs are white coverage → tint by the per-vertex fg
    // colour (also premultiplied); colour glyphs (emoji, vIsColor≈1) keep their own texture colour.
    vec4 tex = texture(atlas, vUv);
    vec4 coverage = vColor * tex.a;
    fragColor = mix(coverage, tex, vIsColor) * qt_Opacity;
}
