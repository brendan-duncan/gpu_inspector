#version 450
#extension GL_EXT_multiview : enable
// One triangle, both eyes in one pass: gl_ViewIndex picks the eye's matrix.

layout(push_constant) uniform Push {
    mat4 mvp[2];
} pc;

layout(location = 0) out vec3 fragColor;

const vec3 positions[3] = vec3[](vec3(-0.5, -0.4, 0.0), vec3(0.5, -0.4, 0.0), vec3(0.0, 0.5, 0.0));
const vec3 colors[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));

void main() {
    gl_Position = pc.mvp[gl_ViewIndex] * vec4(positions[gl_VertexIndex], 1.0);
    fragColor = colors[gl_VertexIndex];
}
