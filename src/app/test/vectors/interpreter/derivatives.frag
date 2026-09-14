#version 450
layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D tex;

void main() {
    vec2 dx = dFdx(inUV);
    vec2 dy = dFdy(inUV);
    float w = fwidth(inUV.x * 4.0);
    outColor = vec4(dx.x * 1000.0, dy.y * 1000.0, w * 1000.0, texture(tex, inUV).r);
}
