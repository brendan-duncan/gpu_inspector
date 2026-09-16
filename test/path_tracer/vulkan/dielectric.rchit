#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_GOOGLE_include_directive : require

// dielectric::scatter: refraction, with Schlick's approximation choosing reflection instead.
#include "common.glsl"

layout(location = 0) rayPayloadInEXT Payload payload;
hitAttributeEXT vec3 outwardNormal;

float Reflectance(float cosine, float refractionIndex) {
    float r0 = (1.0 - refractionIndex) / (1.0 + refractionIndex);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

void main() {
    Sphere s = HIT_SPHERE;
    vec3 direction = gl_WorldRayDirectionEXT;
    bool frontFace = dot(direction, outwardNormal) < 0.0;
    vec3 normal = frontFace ? outwardNormal : -outwardNormal;
    float ri = frontFace ? 1.0 / s.param : s.param;

    vec3 unitDirection = normalize(direction);
    float cosTheta = min(dot(-unitDirection, normal), 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    bool cannotRefract = ri * sinTheta > 1.0;
    vec3 scattered;
    if (cannotRefract || Reflectance(cosTheta, ri) > RandomFloat(payload.rng))
        scattered = reflect(unitDirection, normal);
    else
        scattered = refract(unitDirection, normal, ri);

    payload.origin = gl_WorldRayOriginEXT + gl_HitTEXT * direction;
    payload.direction = scattered;
    payload.color = vec3(1.0);
    payload.state = kScattered;
}
