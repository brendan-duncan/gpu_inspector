// Derivatives across the pixel quad, for the MSL interpreter's DerivativeSource path.
#include <metal_stdlib>
using namespace metal;

struct Varyings {
    float4 position [[position]];
    float2 uv;
};

fragment float4 gradients(Varyings in [[stage_in]],
                          texture2d<float> source [[texture(0)]],
                          sampler smp [[sampler(0)]]) {
    float2 dx = dfdx(in.uv);
    float2 dy = dfdy(in.uv);
    float2 w = fwidth(in.uv);
    // The implicit level of detail comes from the same quad, so this is sampled without one.
    float4 texel = source.sample(smp, in.uv);
    return float4(dx.x, dy.y, w.x, texel.r);
}
