// What every stage of the path tracer shares: the bindings, the payload and the random numbers.
// The layouts match test/path_tracer/scene.h.

struct Sphere {
    vec3 center;
    float radius;
    vec3 albedo;
    float param;   // metal: fuzz; dielectric: index of refraction
};

layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(set = 0, binding = 1, rgba8) uniform image2D outputImage;
layout(set = 0, binding = 2, rgba32f) uniform image2D accumulation;
layout(set = 0, binding = 3) readonly buffer Spheres { Sphere spheres[]; };
layout(set = 0, binding = 4) uniform Camera {
    vec3 center;
    float defocusAngle;
    vec3 pixel00;
    float pad0;
    vec3 pixelDeltaU;
    float pad1;
    vec3 pixelDeltaV;
    float pad2;
    vec3 defocusDiskU;
    float pad3;
    vec3 defocusDiskV;
    float pad4;
} camera;

layout(push_constant) uniform Frame {
    uint frame;
    uint samplesPerFrame;
    uint maxDepth;
    uint accumulate;
} params;

// What a hit or a miss hands back to the ray generation shader, which walks the path itself
// rather than recursing (the pipeline's recursion depth is 1).
const uint kMissed = 0u;      // color is the sky
const uint kScattered = 1u;   // color is the attenuation; continue along origin, direction
const uint kAbsorbed = 2u;    // the path ends black

struct Payload {
    vec3 color;
    uint state;
    vec3 origin;
    uint rng;
    vec3 direction;
    float pad;
};

// PCG (Jarzynski and Olano, "Hash Functions for GPU Rendering").
uint Pcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// [0, 1)
float RandomFloat(inout uint state) {
    return float(Pcg(state) >> 8u) / 16777216.0;
}

vec3 RandomUnitVector(inout uint state) {
    float z = RandomFloat(state) * 2.0 - 1.0;
    float a = RandomFloat(state) * 6.28318530718;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return vec3(r * cos(a), r * sin(a), z);
}

vec2 RandomInUnitDisk(inout uint state) {
    float r = sqrt(RandomFloat(state));
    float a = RandomFloat(state) * 6.28318530718;
    return vec2(r * cos(a), r * sin(a));
}

bool NearZero(vec3 v) {
    return all(lessThan(abs(v), vec3(1e-8)));
}

// The sphere a hit is on: the instance's custom index is its material's first sphere. A macro,
// since the built-ins it reads only exist in the intersection and hit stages.
#define HIT_SPHERE spheres[gl_InstanceCustomIndexEXT + gl_PrimitiveID]
