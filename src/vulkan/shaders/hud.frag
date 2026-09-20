#version 450
// The in-app HUD's fragment shader: the interpolated color and nothing else. Every shape the
// HUD draws is a flat rectangle (hud_text.h expands the text to one rectangle per lit font
// pixel), so there is no sampling to do.

layout(location = 0) in vec4 vColor;
layout(location = 0) out vec4 oColor;

void main() {
    oColor = vColor;
}
