// The in-app HUD's shaders (see src/vulkan/src/hud_text.h, which builds the rectangles).
//
// One instance per rectangle, four vertices as a triangle strip. Nothing is sampled and nothing is
// bound but the vertex buffer and two root constants: the colour travels with the instance, so the
// HUD needs no descriptor heap for its own drawing.
//
// The one difference from the Vulkan version of these shaders is the y flip below: HUD rectangles
// are in pixels from the top-left, which is the direction Vulkan's clip space y already runs but
// the opposite of D3D's.

cbuffer Push : register(b0) {
    float2 invTargetSize;   // 1 / (width, height) of the back buffer
    float2 pad;
};

struct VSInput {
    float4 rect : RECT;      // x, y, w, h in pixels, per instance
    float4 color : COLOR;    // straight (non-premultiplied) RGBA, per instance
    uint vertexId : SV_VertexID;
};

struct VSOutput {
    float4 position : SV_Position;
    float4 color : COLOR;
};

VSOutput VSMain(VSInput input) {
    // 0 -> (0,0), 1 -> (1,0), 2 -> (0,1), 3 -> (1,1): a strip covering the rectangle.
    float2 corner = float2(input.vertexId & 1, (input.vertexId >> 1) & 1);
    float2 px = input.rect.xy + corner * input.rect.zw;
    float2 ndc = px * invTargetSize * 2.0 - 1.0;
    VSOutput output;
    output.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target {
    return input.color;
}
