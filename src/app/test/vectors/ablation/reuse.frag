#version 450
// A temporary reused for two unrelated reads, the way engine-generated shaders reuse a few: the
// discard depends only on the first texture read, so the second texture can still be taken out.
layout(binding = 0) uniform sampler2D albedo;
layout(binding = 1) uniform sampler2D mask;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 t = texture(mask, uv);
    if (t.a < 0.5) discard;
    t = texture(albedo, uv);
    outColor = t;
}
