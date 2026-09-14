#version 450
layout(location = 0) in vec3 inColor;
layout(location = 1) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform Params {
    mat4 transform;
    vec3 tint;
    float scale;
    float weights[3];
    int mode;
} params;

layout(push_constant) uniform Push {
    float bias;
    uint flags;
} pc;

layout(set = 0, binding = 1) uniform sampler2D tex;

struct Light {
    vec3 dir;
    float power;
};

float shade(Light l, vec3 n) {
    return max(dot(normalize(n), -l.dir), 0.0) * l.power;
}

void main() {
    Light light = Light(vec3(0.0, -1.0, 0.0), params.scale);
    float sum = 0.0;
    for (int i = 0; i < 3; i++) {
        sum += params.weights[i] * float(i + 1);
    }
    vec4 texel = textureLod(tex, inUV, 0.0);
    vec3 color = inColor * params.tint;
    switch (params.mode) {
    case 0:
        color *= shade(light, vec3(0.0, 1.0, 0.0));
        break;
    case 1:
        color = texel.rgb;
        break;
    default:
        color = vec3(sum);
        break;
    }
    if ((pc.flags & 1u) != 0u) {
        color += vec3(pc.bias);
    }
    vec4 p = params.transform * vec4(color, 1.0);
    if (p.x < -100.0) {
        discard;
    }
    outColor = vec4(p.xyz, texel.a);
}
