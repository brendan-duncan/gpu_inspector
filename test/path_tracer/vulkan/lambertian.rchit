#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

// lambertian::scatter
#include "common.glsl"

layout(location = 0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec3 outwardNormal;

void main() {
    Sphere s = HIT_SPHERE;
    vec3 direction = gl_WorldRayDirectionEXT;
    bool frontFace = dot(direction, outwardNormal) < 0.0;
    vec3 normal = frontFace ? outwardNormal : -outwardNormal;

    vec3 scatterDirection = normal + RandomUnitVector(payload.rng);
    if (NearZero(scatterDirection)) scatterDirection = normal;

    payload.origin = gl_WorldRayOriginEXT + gl_HitTEXT * direction;
    payload.direction = scatterDirection;
    payload.color = s.albedo;
    payload.state = kScattered;
}
