#version 450
// The shader debugger's tessellation evaluation stage: the patch's per-vertex and patch inputs, the
// tessellation levels and gl_TessCoord.
layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in float doubled[];
layout(location = 2) patch in float total;

layout(location = 0) out vec4 result;

void main() {
    const vec3 c = gl_TessCoord;
    gl_Position = c.x * gl_in[0].gl_Position + c.y * gl_in[1].gl_Position + c.z * gl_in[2].gl_Position;
    result = vec4(c.x * doubled[0] + c.y * doubled[1] + c.z * doubled[2], total, gl_TessLevelOuter[1], float(gl_PrimitiveID));
}
