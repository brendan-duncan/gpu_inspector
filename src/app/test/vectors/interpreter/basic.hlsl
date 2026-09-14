cbuffer Params : register(b0) {
    float4x4 M;
    float3 tint;
    float amount;
};
Texture2D tex : register(t1);
SamplerState samp : register(s2);

struct PSIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target {
    float3 c = tint * amount;
    c = lerp(c, float3(1, 0, 0), saturate(i.uv.x));
    return float4(mul(M, float4(c, 1)).xyz, tex.SampleLevel(samp, i.uv, 0).g);
}
