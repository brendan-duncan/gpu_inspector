#version 450

// --geometry: each of the cube's triangles as it is, and a copy of it shifted right by 0.3 of w. A
// geometry shader that emits more than it takes in, for the mesh output view's GS Out.

layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;

layout(location = 0) in vec3 inColor[];
layout(location = 1) in vec2 inUV[];

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV;

void main() {
    for (int copy = 0; copy < 2; ++copy) {
        for (int v = 0; v < 3; ++v) {
            gl_Position = gl_in[v].gl_Position + vec4(0.3 * float(copy) * gl_in[v].gl_Position.w, 0.0, 0.0, 0.0);
            fragColor = inColor[v];
            fragUV = inUV[v];
            EmitVertex();
        }
        EndPrimitive();
    }
}
