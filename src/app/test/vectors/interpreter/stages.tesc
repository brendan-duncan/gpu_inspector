#version 450
// The shader debugger's tessellation control stage: each invocation writes its own vertex, and after
// barrier() reads its neighbour's, which only the barrier makes defined.
layout(vertices = 3) out;

layout(location = 0) in float inValue[];

layout(location = 0) out float doubled[];
layout(location = 1) out float neighbour[];
layout(location = 2) patch out float total;

void main() {
    doubled[gl_InvocationID] = inValue[gl_InvocationID] * 2.0;
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    barrier();
    neighbour[gl_InvocationID] = doubled[(gl_InvocationID + 1) % 3];
    if (gl_InvocationID == 0) {
        total = doubled[0] + doubled[1] + doubled[2];
        gl_TessLevelOuter[0] = 3.0;
        gl_TessLevelOuter[1] = 4.0;
        gl_TessLevelOuter[2] = 5.0;
        gl_TessLevelInner[0] = 6.0;
    }
}
