#version 450
// The shader debugger's geometry stage: per-vertex inputs (gl_in[] and a located array), the
// primitive and invocation ids, and two strips emitted, the second longer than a triangle.
layout(triangles, invocations = 2) in;
layout(triangle_strip, max_vertices = 7) out;

layout(location = 0) in vec3 inColor[];

layout(location = 0) out vec3 color;
layout(location = 1) out float tag;

void main() {
    for (int v = 0; v < 3; ++v) {
        gl_Position = gl_in[v].gl_Position;
        color = inColor[v];
        tag = float(gl_PrimitiveIDIn * 10 + gl_InvocationID);
        EmitVertex();
    }
    EndPrimitive();
    // A strip of four: two triangles.
    for (int v = 0; v < 4; ++v) {
        gl_Position = gl_in[v % 3].gl_Position + vec4(1.0, 0.0, 0.0, 0.0);
        color = inColor[v % 3] * 2.0;
        tag = -1.0;
        EmitVertex();
    }
    EndPrimitive();
}
