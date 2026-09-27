#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;

layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 color;
    vec4 params; // x: alpha cut
} pc;

layout(location = 0) out vec4 outColor;

void main()
{
    // Alpha cut is the GS alpha test GREATER against 0, where alpha 1.0 is
    // 0x80, so the smallest alpha that passes is 1/128.
    if (pc.params.x != 0.0 && vColor.a < 1.0 / 128.0)
        discard;
    outColor = vColor;
}
