#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

// metal::scatter: a mirror reflection, blurred by the fuzz.
#include "common.glsl"

layout(location = 0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec3 outwardNormal;

void main() {
    Sphere s = HIT_SPHERE;
    vec3 direction = gl_WorldRayDirectionEXT;
    bool frontFace = dot(direction, outwardNormal) < 0.0;
    vec3 normal = frontFace ? outwardNormal : -outwardNormal;

    vec3 reflected = normalize(reflect(direction, normal)) + s.param * RandomUnitVector(payload.rng);

    payload.origin = gl_WorldRayOriginEXT + gl_HitTEXT * direction;
    payload.direction = reflected;
    payload.color = s.albedo;
    // A fuzzed reflection that points into the surface is absorbed.
    payload.state = dot(reflected, normal) > 0.0 ? kScattered : kAbsorbed;
}
