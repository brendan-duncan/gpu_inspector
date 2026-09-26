#version 450

// --heavy-vertex: cube.vert with vertex work of a known relative cost in named functions, for
// measuring a vertex shader by ablation. wobble() does far more than shade(); both change what the
// vertex writes, so neither can be left out by the compiler.

layout(set = 0, binding = 0) uniform Uniforms {
    mat4 mvp;
} u;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;

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
    vec3 position = inPosition * (1.0 + 1e-4 * wobble(inPosition));
    gl_Position = u.mvp * vec4(position, 1.0);
    fragColor = shade(inColor);
    fragUV = inUV;
}
