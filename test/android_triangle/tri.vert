#version 450
// A ring of small triangles: gl_InstanceIndex places each one, one instanced draw renders all.

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec3 fragColor;

const float kTriangles = 48.0;
const vec3 positions[3] = vec3[](vec3(-0.5, -0.4, 0.0), vec3(0.5, -0.4, 0.0), vec3(0.0, 0.5, 0.0));
const vec3 colors[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));

void main() {
    float a = float(gl_InstanceIndex) * 6.2831853 / kTriangles;
    vec3 p = positions[gl_VertexIndex] * 0.15;
    p.xy += vec2(cos(a), sin(a)) * 0.9;
    gl_Position = pc.mvp * vec4(p, 1.0);
    fragColor = mix(colors[gl_VertexIndex], vec3(1.0), 0.5 * (0.5 + 0.5 * sin(a * 3.0)));
}
