#version 450

// --alpha-test: cube.frag with the checker's dark squares discarded, so every face is half holes
// (alpha-tested geometry, as foliage and fences are drawn).

layout(set = 0, binding = 1) uniform sampler2D checker;

layout(push_constant) uniform Push {
    float tint;
} pc;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    float c = texture(checker, fragUV).r;
    if (c < 0.5)
        discard;
    outColor = vec4(mix(fragColor, fragColor * c, pc.tint), 1.0);
}
