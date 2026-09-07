#version 450

layout(set = 0, binding = 1) uniform sampler2D checker;

layout(push_constant) uniform Push {
    float tint;
} pc;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

void main() {
    float c = texture(checker, fragUV).r;
    outColor = vec4(mix(fragColor, fragColor * c, pc.tint), 1.0);
}
