#version 450

// --tessellation: the patch's corners interpolated at the tessellator's coordinate, which is written
// out too so a replay's DS Out can be checked against the corners it came from.

layout(triangles, equal_spacing, ccw) in;

layout(location = 0) in vec3 inColor[];
layout(location = 1) in vec2 inUV[];

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 tessCoord;

void main() {
    const vec3 c = gl_TessCoord;
    gl_Position = c.x * gl_in[0].gl_Position + c.y * gl_in[1].gl_Position + c.z * gl_in[2].gl_Position;
    fragColor = c.x * inColor[0] + c.y * inColor[1] + c.z * inColor[2];
    fragUV = c.x * inUV[0] + c.y * inUV[1] + c.z * inUV[2];
    tessCoord = c;
}
