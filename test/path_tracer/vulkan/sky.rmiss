#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

// ray_color's background: white blended to sky blue by the ray's height.
#include "common.glsl"

layout(location = 0) rayPayloadInEXT Payload payload;

void main() {
    vec3 unitDirection = normalize(gl_WorldRayDirectionEXT);
    float a = 0.5 * (unitDirection.y + 1.0);
    payload.color = (1.0 - a) * vec3(1.0) + a * vec3(0.5, 0.7, 1.0);
    payload.state = kMissed;
}
