#version 450
// The in-app HUD's vertex shader (see src/vulkan/src/hud_text.h).
//
// One instance per rectangle, four vertices drawn as a triangle strip. Nothing is sampled and
// nothing is bound but the vertex buffer: the color travels with the instance, so the HUD needs
// no descriptor set, no sampler and no font texture.
//
// Pixel coordinates run from the top-left of the swapchain image, which is also the direction
// Vulkan's clip space y runs, so the mapping below needs no flip.

layout(push_constant) uniform Push {
    vec2 invTargetSize;   // 1 / (width, height) of the image being drawn into
} pc;

layout(location = 0) in vec4 iRect;    // x, y, w, h in pixels
layout(location = 1) in vec4 iColor;   // straight (non-premultiplied) RGBA

layout(location = 0) out vec4 vColor;

void main() {
    // 0 -> (0,0), 1 -> (1,0), 2 -> (0,1), 3 -> (1,1): a strip covering the rectangle.
    vec2 corner = vec2(gl_VertexIndex & 1, (gl_VertexIndex >> 1) & 1);
    vec2 px = iRect.xy + corner * iRect.zw;
    gl_Position = vec4(px * pc.invTargetSize * 2.0 - 1.0, 0.0, 1.0);
    vColor = iColor;
}
