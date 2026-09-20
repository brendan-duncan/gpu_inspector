// The cube's vertex and pixel shaders (dxinsp_triangle). Compiled at build time with
// dxc -Zi -Qembed_debug, so the DXIL carries this text for the inspector's Source view.

// Root parameter 0, a descriptor table: CBV b0 and SRV t0.
cbuffer Cube : register(b0) {
    float4x4 viewProj;
    float4x4 model;
};

// Root parameter 1, root constants.
cbuffer Frame : register(b1) {
    float time;
    uint flags;      // bit 0: swap the red and blue channels
};

Texture2D checker : register(t0);
SamplerState pointSampler : register(s0);   // static sampler

struct VSInput {
    float3 position : POSITION;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
    uint instance : SV_InstanceID;
};

struct PSInput {
    float4 position : SV_Position;
    float3 color : COLOR;
    float2 uv : TEXCOORD;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 world = mul(model, float4(input.position, 1.0));
    // The two instances sit side by side, each spinning about its own center.
    world.x += ((float)input.instance * 2.0 - 1.0) * 0.9;
    output.position = mul(viewProj, world);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

float4 PSMain(PSInput input) : SV_Target {
    float4 texel = checker.Sample(pointSampler, input.uv);
    float pulse = 0.75 + 0.25 * sin(time * 2.0);
    float3 color = texel.rgb * input.color * pulse;
    if (flags & 1) color = color.bgr;
    return float4(color, 1.0);
}
