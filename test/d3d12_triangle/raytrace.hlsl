// --ray-tracing: the DXR counterpart of test/triangle's rt.rgen / rt.rmiss / rt.rchit.
//
// One ray per pixel of a 256x256 UAV, straight down -z through two triangles the scene's two
// instances hold. Compiled as one library (lib_6_3) with four exports, which is what a DXR state
// object is built out of: a raygen, a miss, and two closest hits that differ only in what they
// write, so the shader binding table has two hit group records and picking the wrong one is
// visible in the output.
//
// The shaders are deliberately declared with [shader("...")] and no explicit export list in the
// state object, so the capture library's identifier hook is exercised alongside the names the
// description gives it.

RaytracingAccelerationStructure scene : register(t0);
RWTexture2D<float4> target : register(u0);

#ifdef LOCAL_ROOT
// --local-root (raytrace_local.hlsl): the tinted hit group's local root signature, in space 1 so it
// cannot collide with the global one: [0] a root constant, [1] a root CBV, [2] a table of one CBV.
cbuffer LocalScale : register(b0, space1) { float localScale; };
cbuffer LocalTint : register(b1, space1) { float4 localTint; };
cbuffer LocalTable : register(b2, space1) { float4 localTableColor; };
#endif

struct Payload {
    float3 color;
};

[shader("raygeneration")]
void RayGen() {
    uint2 pixel = DispatchRaysIndex().xy;
    float2 uv = (float2(pixel) + 0.5) / float2(DispatchRaysDimensions().xy) * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin = float3(uv.x, -uv.y, -1.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 10.0;

    Payload payload;
    payload.color = float3(0.0, 0.0, 0.0);
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    target[pixel] = float4(payload.color, 1.0);
}

[shader("miss")]
void Miss(inout Payload payload) {
    payload.color = float3(0.1, 0.1, 0.2);
}

// The barycentrics of the hit, as test/triangle's closest hit writes them.
[shader("closesthit")]
void ClosestHit(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    float2 b = attributes.barycentrics;
    payload.color = float3(1.0 - b.x - b.y, b.x, b.y);
}

// The second hit group, reached only through the second instance's
// InstanceContributionToHitGroupIndex: a flat color, so which record ran is unmistakable.
[shader("closesthit")]
void ClosestHitTinted(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    float2 b = attributes.barycentrics;
#ifdef LOCAL_ROOT
    // Every term from a local root argument: a wrong address or handle in the record shows here.
    payload.color = localTint.rgb * localScale + localTableColor.rgb * (1.0 - b.x - b.y);
#else
    payload.color = float3(0.9, 0.5 * (1.0 - b.x - b.y), 0.2);
#endif
}
