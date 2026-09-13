#version 450
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 vUV;
layout(location = 1) flat out int vIndex;

layout(set = 0, binding = 0) uniform Camera {
    mat4 viewProj;
    mat3 normalMat;
} cam;

void main() {
    vec3 offset = cam.normalMat * vec3(1.0, 0.0, 0.0);
    gl_Position = cam.viewProj * vec4(pos + offset * float(gl_VertexIndex), 1.0);
    vUV = uv.yx;
    vIndex = gl_VertexIndex + gl_InstanceIndex;
}
