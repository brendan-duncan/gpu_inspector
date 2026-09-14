#version 450
// --heavy: the cube's fragment shader with work of a known relative cost in named functions, for
// the Shader Flame Graph's measurements by ablation. fbm() does far more than blurred(), and
// main() itself almost nothing.

layout(set = 0, binding = 1) uniform sampler2D checker;

layout(push_constant) uniform Push {
    float tint;
} pc;

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

// Value noise summed over octaves: four hashes an octave.
float fbm(vec2 p) {
    float sum = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 480; ++i) {
        vec2 cell = floor(p);
        vec2 f = fract(p);
        float a = hash(cell);
        float b = hash(cell + vec2(1.0, 0.0));
        float c = hash(cell + vec2(0.0, 1.0));
        float d = hash(cell + vec2(1.0, 1.0));
        vec2 u = f * f * (3.0 - 2.0 * f);
        sum += amplitude * mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
        p = p * 2.03 + vec2(0.17, 0.31);
        amplitude *= 0.5;
    }
    return sum;
}

// A box blur of the checker texture: sixteen samples.
vec3 blurred(vec2 uv) {
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            sum += texture(checker, uv + vec2(x, y) * 0.01).rgb;
        }
    }
    return sum / 16.0;
}

void main() {
    float n = fbm(fragUV * 8.0);
    vec3 tex = blurred(fragUV);
    vec3 color = mix(fragColor, tex * (0.5 + n), pc.tint);
    outColor = vec4(color, 1.0);
}
