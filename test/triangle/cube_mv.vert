#version 450
#extension GL_EXT_multiview : require

// --multiview: cube.vert drawing both views of a two-layer target at once, each view shifted the
// other way, the way an XR runtime's stereo pass draws its two eyes.

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
    gl_Position.x += (gl_ViewIndex == 0 ? -0.12 : 0.12) * gl_Position.w;
    fragColor = inColor;
    fragUV = inUV;
}
