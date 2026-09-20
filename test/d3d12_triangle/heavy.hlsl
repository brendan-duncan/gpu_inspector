// --heavy: the cube's pixel shader with work of a known relative cost in named functions, for the
// Shader Flame Graph's measurements by ablation (the counterpart of test/triangle/heavy.frag).
// Fbm() does far more than Blurred(), and PSMain() itself almost nothing. Bound as cube.hlsl is.

cbuffer Frame : register(b1) {
    float time;
    uint flags;
};

Texture2D checker : register(t0);
SamplerState pointSampler : register(s0);

struct PSInput {
    float4 position : SV_Position;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
};

float Hash(float2 p) {
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

// Value noise summed over octaves: four hashes an octave.
float Fbm(float2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 480; ++i) {
        float2 cell = floor(p);
        float2 f = frac(p);
        float a = Hash(cell);
        float b = Hash(cell + float2(1.0, 0.0));
        float c = Hash(cell + float2(0.0, 1.0));
        float d = Hash(cell + float2(1.0, 1.0));
        float2 u = f * f * (3.0 - 2.0 * f);
        sum += amplitude * lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
        p = p * 2.03 + float2(0.17, 0.31);
        amplitude *= 0.5;
    }
    return sum;
}

// A box blur of the checker texture: sixteen samples.
float3 Blurred(float2 uv) {
    float3 sum = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            sum += checker.Sample(pointSampler, uv + float2(x, y) * 0.01).rgb;
        }
    }
    return sum / 16.0;
}

float4 PSMain(PSInput input) : SV_Target {
    float n = Fbm(input.uv * 8.0);
    float3 tex = Blurred(input.uv);
    float pulse = 0.75 + 0.25 * sin(time * 2.0);
    float3 color = lerp(input.color, tex * (0.5 + n), pulse);
    return float4(color, 1.0);
}
