#version 450
// The "slow" package's fragment shader: deliberately wasteful, so the inspector's shader
// analysis has something to flag (expensive builtins in a loop, loop-invariant computations
// repeated every iteration, an integer division). The output is the plain color with a faint
// glow and banding.

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    float glow = 0.0;
    for (int i = 0; i < 32; ++i) {
        // Loop-invariant: they depend on the fragment only, yet are computed 32 times.
        float r = length(gl_FragCoord.xy * 0.001);
        float scale = fragColor.g * 2.0 + fragColor.b * 0.5 + 0.25;
        // Expensive builtins per iteration.
        glow += pow(sin(r * 3.0 + float(i) * 0.2), 2.0) * scale / 32.0;
    }
    // Integer division by a value only known at run time.
    int stripes = int(gl_FragCoord.x) / (int(gl_FragCoord.y) / 7 + 1);
    outColor = vec4(fragColor * (0.7 + 0.3 * glow) + float(stripes % 2) * 0.02, 1.0);
}
