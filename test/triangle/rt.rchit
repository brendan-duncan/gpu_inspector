#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 color;
hitAttributeEXT vec2 barycentrics;

void main() {
    color = vec3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);
}
