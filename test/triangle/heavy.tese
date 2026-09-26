#version 450

// --heavy-tessellation: cube.tese with evaluation work of a known relative cost in named functions,
// for measuring a tessellation evaluation shader by ablation. wobble() does far more than shade();
// both change what the point writes, so neither can be left out by the compiler.

layout(triangles, equal_spacing, ccw) in;

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
    const vec3 c = gl_TessCoord;
    vec4 position = c.x * gl_in[0].gl_Position + c.y * gl_in[1].gl_Position + c.z * gl_in[2].gl_Position;
    gl_Position = vec4(position.xyz * (1.0 + 1e-4 * wobble(c * 3.0)), position.w);
    fragColor = shade(c.x * inColor[0] + c.y * inColor[1] + c.z * inColor[2]);
    fragUV = c.x * inUV[0] + c.y * inUV[1] + c.z * inUV[2];
}
