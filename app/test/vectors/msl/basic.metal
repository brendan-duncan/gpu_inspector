// Test vector for the MSL interpreter (src/renderer/msl/). Unlike the SPIR-V vectors beside it,
// this needs no offline compiler: the source *is* the input the debugger steps.
//
// Every entry point here is checked in test/msl_interpreter.test.js against the value worked out
// by hand, so what each one computes is meant to be readable rather than realistic.
#include <metal_stdlib>
using namespace metal;

#define SCALE 2.0f
#define TWICE(x) ((x) + (x))

struct Uniforms {
    float4 tint;          //  0
    float3 direction;     // 16 (float3 is sixteen bytes wide and sixteen-aligned)
    float  weight;        // 32
    float2x2 rotation;    // 40 (a float2 column is eight-aligned)
    int    mode;          // 56
    bool   flag;          // 60 (one byte)
};                        // 64 bytes, sixteen-aligned

struct Packed {
    packed_float3 position;   //  0, 12 bytes
    float         scale;      // 12
};

struct VertexIn {
    float2 position [[attribute(0)]];
    float3 colour   [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float3 colour;
    float  fog;
};

constant float3 kAxis = float3(0.0f, 1.0f, 0.0f);
constant int kCounts[4] = { 1, 2, 3, 4 };

static float weighted(float a, float b, float t) {
    return a * (1.0f - t) + b * t;
}

static float weighted(float3 a, float3 b) {
    return dot(a, b);
}

// Scalar and vector arithmetic, swizzles, conversions and the standard library.
kernel void arithmetic(device float *out [[buffer(0)]],
                       constant Uniforms &u [[buffer(1)]],
                       uint i [[thread_position_in_grid]]) {
    float4 t = u.tint;
    float3 rgb = t.rgb * SCALE;
    float2 flipped = t.yx;
    int n = int(u.weight) + kCounts[i % 4];
    float mixed = mix(rgb.x, rgb.z, 0.25f);
    float clamped = clamp(TWICE(u.weight), 0.0f, 1.0f);
    float3 axis = normalize(kAxis + u.direction);
    out[0] = rgb.x + rgb.y + rgb.z;
    out[1] = flipped.x;
    out[2] = float(n);
    out[3] = mixed;
    out[4] = clamped;
    out[5] = length(axis);
    out[6] = weighted(t.x, t.w, 0.5f);
    out[7] = weighted(u.direction, kAxis);
    out[8] = float(u.flag ? 1 : 0);
}

// Control flow: loops, an early break, a switch and a call.
kernel void control(device int *out [[buffer(0)]],
                    constant Uniforms &u [[buffer(1)]],
                    uint i [[thread_position_in_grid]]) {
    int total = 0;
    for (int k = 0; k < 8; ++k) {
        if (k == 5) {
            break;
        }
        total += k;
    }
    int doubled = 0;
    while (doubled < 10) {
        doubled = doubled * 2 + 1;
    }
    int picked = 0;
    switch (u.mode) {
        case 0:
            picked = 100;
            break;
        case 1:
        case 2:
            picked = 200;
            break;
        default:
            picked = 300;
            break;
    }
    out[0] = total;
    out[1] = doubled;
    out[2] = picked;
    out[3] = int(i);
}

// Buffer layout: a struct read through `constant &`, a packed struct, and a write back.
kernel void layout(device Packed *out [[buffer(0)]],
                   constant Uniforms &u [[buffer(1)]],
                   const device Packed *in [[buffer(2)]],
                   uint i [[thread_position_in_grid]]) {
    Packed p = in[i];
    p.position = p.position * u.weight;
    p.scale = u.rotation[0][0];
    out[i] = p;
}

// A vertex entry point: `[[stage_in]]` attributes, a built-in and a matrix.
vertex VertexOut transform(VertexIn in [[stage_in]],
                           constant Uniforms &u [[buffer(1)]],
                           uint vid [[vertex_id]]) {
    float2 rotated = u.rotation * in.position;
    VertexOut out;
    out.position = float4(rotated, 0.0f, 1.0f);
    out.colour = in.colour * u.tint.rgb;
    out.fog = float(vid) * u.weight;
    return out;
}

// A fragment entry point: varyings matched by name, a texture and a sampler.
fragment float4 shade(VertexOut in [[stage_in]],
                      texture2d<float> albedo [[texture(0)]],
                      sampler smp [[sampler(0)]],
                      constant Uniforms &u [[buffer(1)]]) {
    float4 texel = albedo.sample(smp, in.position.xy, level(0.0f));
    float3 lit = in.colour * texel.rgb;
    if (in.fog > 0.5f) {
        lit = mix(lit, float3(1.0f), 0.5f);
    }
    return float4(lit, u.tint.a);
}
