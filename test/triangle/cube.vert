#version 450

layout(set = 0, binding = 0) uniform Uniforms {
    mat4 mvp;
} u;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    gl_Position = u.mvp * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
