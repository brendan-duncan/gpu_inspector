// The path tracer's DXR library: every entry point of the pipeline, compiled once as lib_6_3.
// The Direct3D 12 counterpart of test/path_tracer/vulkan's GLSL stages; the layouts match
// test/path_tracer/scene.h.

struct Sphere {
    float3 center;
    float radius;
    float3 albedo;
    float param;   // metal: fuzz; dielectric: index of refraction
};

RaytracingAccelerationStructure Scene : register(t0);
StructuredBuffer<Sphere> Spheres : register(t1);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> Accumulation : register(u1);

cbuffer Camera : register(b0) {
    float3 cameraCenter;
    float defocusAngle;
    float3 pixel00;
    float pad0;
    float3 pixelDeltaU;
    float pad1;
    float3 pixelDeltaV;
    float pad2;
    float3 defocusDiskU;
    float pad3;
    float3 defocusDiskV;
    float pad4;
};

cbuffer Frame : register(b1) {
    uint frameIndex;
    uint samplesPerFrame;
    uint maxDepth;
    uint accumulate;
};

// What a hit or a miss hands back to the ray generation shader, which walks the path itself
// rather than recursing (the pipeline's recursion depth is 1).
static const uint kMissed = 0;      // color is the sky
static const uint kScattered = 1;   // color is the attenuation; continue along origin, direction
static const uint kAbsorbed = 2;    // the path ends black

struct Payload {
    float3 color;
    uint state;
    float3 origin;
    uint rng;
    float3 direction;
    float pad;
};

struct SphereHit {
    float3 outwardNormal;
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

float3 RandomUnitVector(inout uint state) {
    float z = RandomFloat(state) * 2.0 - 1.0;
    float a = RandomFloat(state) * 6.28318530718;
    float r = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(a), r * sin(a), z);
}

float2 RandomInUnitDisk(inout uint state) {
    float r = sqrt(RandomFloat(state));
    float a = RandomFloat(state) * 6.28318530718;
    return float2(r * cos(a), r * sin(a));
}

bool NearZero(float3 v) {
    return all(abs(v) < 1e-8);
}

// The sphere a hit is on: the instance's ID is its material's first sphere.
Sphere HitSphere() {
    return Spheres[InstanceID() + PrimitiveIndex()];
}

float3 FacingNormal(float3 direction, float3 outwardNormal, out bool frontFace) {
    frontFace = dot(direction, outwardNormal) < 0.0;
    return frontFace ? outwardNormal : -outwardNormal;
}

float3 HitPoint() {
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

// ------------------------------------------------------------------------------ ray generation

// camera::get_ray: a ray from the defocus disk through a random point in pixel (i, j).
RayDesc CameraRay(uint2 pixel, inout uint rng) {
    float2 offset = float2(RandomFloat(rng), RandomFloat(rng)) - 0.5;
    float3 pixelSample = pixel00 + (pixel.x + offset.x) * pixelDeltaU + (pixel.y + offset.y) * pixelDeltaV;
    RayDesc ray;
    ray.Origin = cameraCenter;
    if (defocusAngle > 0.0) {
        float2 p = RandomInUnitDisk(rng);
        ray.Origin += p.x * defocusDiskU + p.y * defocusDiskV;
    }
    ray.Direction = pixelSample - ray.Origin;
    ray.TMin = 0.001;   // the book's cure for shadow acne
    ray.TMax = 1.0e30;
    return ray;
}

// camera::ray_color, with the recursion unrolled into a loop.
float3 RayColor(RayDesc ray, inout uint rng) {
    float3 throughput = 1.0;
    Payload payload = (Payload)0;
    payload.rng = rng;
    for (uint depth = 0; depth < maxDepth; ++depth) {
        TraceRay(Scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, payload);
        if (payload.state == kMissed) {
            rng = payload.rng;
            return throughput * payload.color;
        }
        if (payload.state == kAbsorbed) break;
        throughput *= payload.color;
        ray.Origin = payload.origin;
        ray.Direction = payload.direction;
    }
    rng = payload.rng;
    return 0.0;
}

[shader("raygeneration")]
void PathRayGen() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint rng = (pixel.x * 1973u + pixel.y * 9277u + frameIndex * 26699u) | 1u;

    float3 color = 0.0;
    for (uint s = 0; s < samplesPerFrame; ++s) {
        RayDesc ray = CameraRay(pixel, rng);
        color += RayColor(ray, rng);
    }
    color /= float(samplesPerFrame);

    // A running mean over every frame since the last reset: frame n has weight 1 / (n + 1).
    if (accumulate != 0 && frameIndex > 0) {
        float3 previous = Accumulation[pixel].rgb;
        color = lerp(previous, color, 1.0 / float(frameIndex + 1));
    }
    Accumulation[pixel] = float4(color, 1.0);

    // write_color: gamma 2.
    Output[pixel] = float4(sqrt(saturate(color)), 1.0);
}

// ------------------------------------------------------------------------------ miss

// ray_color's background: white blended to sky blue by the ray's height.
[shader("miss")]
void SkyMiss(inout Payload payload) {
    float3 unitDirection = normalize(WorldRayDirection());
    float a = 0.5 * (unitDirection.y + 1.0);
    payload.color = (1.0 - a) * float3(1.0, 1.0, 1.0) + a * float3(0.5, 0.7, 1.0);
    payload.state = kMissed;
}

// ------------------------------------------------------------------------------ intersection

// sphere::hit, for every material's bounding boxes. Reports the nearer root when it lies in the
// ray's interval and the farther one otherwise (a ray leaving a glass sphere).
[shader("intersection")]
void SphereIntersection() {
    Sphere s = HitSphere();
    float3 origin = ObjectRayOrigin();
    float3 direction = ObjectRayDirection();

    float3 oc = s.center - origin;
    float a = dot(direction, direction);
    float h = dot(direction, oc);
    float c = dot(oc, oc) - s.radius * s.radius;
    float discriminant = h * h - a * c;
    if (discriminant < 0.0) return;
    float sqrtd = sqrt(discriminant);

    // ReportHit refuses a t outside [RayTMin, the closest hit so far].
    SphereHit hit;
    float root = (h - sqrtd) / a;
    hit.outwardNormal = (origin + root * direction - s.center) / s.radius;
    if (ReportHit(root, 0, hit)) return;
    root = (h + sqrtd) / a;
    hit.outwardNormal = (origin + root * direction - s.center) / s.radius;
    ReportHit(root, 0, hit);
}

// ------------------------------------------------------------------------------ closest hit

// lambertian::scatter
[shader("closesthit")]
void LambertianHit(inout Payload payload, SphereHit hit) {
    Sphere s = HitSphere();
    bool frontFace;
    float3 normal = FacingNormal(WorldRayDirection(), hit.outwardNormal, frontFace);

    float3 scatterDirection = normal + RandomUnitVector(payload.rng);
    if (NearZero(scatterDirection)) scatterDirection = normal;

    payload.origin = HitPoint();
    payload.direction = scatterDirection;
    payload.color = s.albedo;
    payload.state = kScattered;
}

// metal::scatter: a mirror reflection, blurred by the fuzz.
[shader("closesthit")]
void MetalHit(inout Payload payload, SphereHit hit) {
    Sphere s = HitSphere();
    bool frontFace;
    float3 normal = FacingNormal(WorldRayDirection(), hit.outwardNormal, frontFace);

    float3 reflected = normalize(reflect(WorldRayDirection(), normal)) + s.param * RandomUnitVector(payload.rng);

    payload.origin = HitPoint();
    payload.direction = reflected;
    payload.color = s.albedo;
    // A fuzzed reflection that points into the surface is absorbed.
    payload.state = dot(reflected, normal) > 0.0 ? kScattered : kAbsorbed;
}

float Reflectance(float cosine, float refractionIndex) {
    float r0 = (1.0 - refractionIndex) / (1.0 + refractionIndex);
    r0 = r0 * r0;
    return r0 + (1.0 - r0) * pow(1.0 - cosine, 5.0);
}

// dielectric::scatter: refraction, with Schlick's approximation choosing reflection instead.
[shader("closesthit")]
void DielectricHit(inout Payload payload, SphereHit hit) {
    Sphere s = HitSphere();
    bool frontFace;
    float3 normal = FacingNormal(WorldRayDirection(), hit.outwardNormal, frontFace);
    float ri = frontFace ? 1.0 / s.param : s.param;

    float3 unitDirection = normalize(WorldRayDirection());
    float cosTheta = min(dot(-unitDirection, normal), 1.0);
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    bool cannotRefract = ri * sinTheta > 1.0;
    float3 scattered;
    if (cannotRefract || Reflectance(cosTheta, ri) > RandomFloat(payload.rng))
        scattered = reflect(unitDirection, normal);
    else
        scattered = refract(unitDirection, normal, ri);

    payload.origin = HitPoint();
    payload.direction = scattered;
    payload.color = 1.0;
    payload.state = kScattered;
}
