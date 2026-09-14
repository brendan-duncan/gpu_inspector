// A compute shader that refreshes a wave buffer every frame (dxinsp_triangle --compute), so a
// capture has a dispatch, a UAV and compute root constants in it. Nothing reads the result.

// Root parameter 1, root constants.
cbuffer Params : register(b0) {
    float time;
    uint count;
};

// Root parameter 0, a descriptor table with one UAV.
RWStructuredBuffer<float> wave : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x < count) wave[id.x] = sin(time + id.x * 0.05);
}
