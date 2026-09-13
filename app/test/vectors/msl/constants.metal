// Function constants, the shape an engine ships one library of variants in: the branches are
// chosen when the function is built (`newFunctionWithName:constantValues:`), not when it is
// compiled, and a debugged invocation has to take the same ones.
#include <metal_stdlib>
using namespace metal;

constant int   kMode   [[function_constant(0)]];
constant float kAmount [[function_constant(1)]];
constant bool  kEnable [[function_constant(2)]];
// Set by name rather than by index, which is the other spelling an application may use.
constant float3 kBias  [[function_constant(3)]];

struct Uniforms {
    float4 colour;
};

kernel void specialized(device float *out [[buffer(0)]],
                        constant Uniforms &u [[buffer(1)]],
                        uint i [[thread_position_in_grid]]) {
    float3 colour = u.colour.rgb;
    if (kEnable) {
        colour = mix(colour, float3(1.0f), kAmount);
    }
    switch (kMode) {
        case 1: colour = colour.bgr; break;
        case 2: colour = 1.0f - colour; break;
        default: break;
    }
    // A constant the application may not have set at all: the shader asks before reading it.
    if (is_function_constant_defined(kBias)) {
        colour += kBias;
    }
    out[0] = colour.r;
    out[1] = colour.g;
    out[2] = colour.b;
    out[3] = is_function_constant_defined(kMode) ? 1.0f : 0.0f;
    out[4] = float(i);
}
