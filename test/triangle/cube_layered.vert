#version 450
#extension GL_ARB_shader_viewport_layer_array : require

// --layered: cube.vert drawn as two instances, each into the layer of its index of a two-layer
// framebuffer (gl_Layer from the vertex shader, VK_EXT_shader_viewport_index_layer), shifted the
// other way: layered rendering as single-pass shadow cascades and cube maps do it.

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
    gl_Position.x += (gl_InstanceIndex == 0 ? -0.12 : 0.12) * gl_Position.w;
    gl_Layer = gl_InstanceIndex;
    fragColor = inColor;
    fragUV = inUV;
}
