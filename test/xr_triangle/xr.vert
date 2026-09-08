#version 450
#extension GL_EXT_multiview : enable
// A ring of small triangles, both eyes in one pass: gl_ViewIndex picks the eye's matrix (0 in
// a pass without multiview, where the application pushes that eye's matrix into both slots).
// gl_InstanceIndex places the triangle on the ring: one instanced draw renders the whole ring,
// or one draw per triangle with firstInstance set (the "slow" package).

layout(push_constant) uniform Push {
    mat4 mvp[2];
} pc;

layout(location = 0) out vec3 fragColor;

const float kTriangles = 48.0;
const vec3 positions[3] = vec3[](vec3(-0.5, -0.4, 0.0), vec3(0.5, -0.4, 0.0), vec3(0.0, 0.5, 0.0));
const vec3 colors[3] = vec3[](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2), vec3(0.2, 0.4, 1.0));

void main() {
    float a = float(gl_InstanceIndex) * 6.2831853 / kTriangles;
    vec3 p = positions[gl_VertexIndex] * 0.15;
    p.xy += vec2(cos(a), sin(a)) * 0.9;
    gl_Position = pc.mvp[gl_ViewIndex] * vec4(p, 1.0);
    fragColor = mix(colors[gl_VertexIndex], vec3(1.0), 0.5 * (0.5 + 0.5 * sin(a * 3.0)));
}
