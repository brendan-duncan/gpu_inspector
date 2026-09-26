#version 450

// --heavy-geometry: cube.geom's pass-through with geometry work of a known relative cost in named
// functions, for measuring a geometry shader by ablation. wobble() does far more than shade(); both
// change what the primitive emits, so neither can be left out by the compiler.

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(location = 0) in vec3 inColor[];
layout(location = 1) in vec2 inUV[];

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV;

float hash(vec3 p) {
    return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

// Noise summed over many octaves: a displacement too small to see.
float wobble(vec3 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 2000; ++i) {
        sum += amplitude * hash(floor(p) + fract(p) * 0.5);
        p = fract(p * 1.93 + vec3(0.17, 0.31, 0.23)) * 7.0;   // bounded, so it never overflows
        amplitude *= 0.99;
    }
    return sum;
}

// A few multiplies: next to nothing.
vec3 shade(vec3 c) {
    return c * 0.9 + 0.1;
}

void main() {
    // Once a primitive, from its first corner.
    float scale = 1.0 + 1e-4 * wobble(gl_in[0].gl_Position.xyz);
    for (int v = 0; v < 3; ++v) {
        gl_Position = vec4(gl_in[v].gl_Position.xyz * scale, gl_in[v].gl_Position.w);
        fragColor = shade(inColor[v]);
        fragUV = inUV[v];
        EmitVertex();
    }
    EndPrimitive();
}
