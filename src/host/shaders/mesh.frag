#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 matColor;
    vec4 uvRows;
    vec2 uvOffset;
    int boneBase;
    int lightBase;
    uint flags;
} pc;

layout(location = 0) out vec4 outColor;

const uint kFlagAlphaCut = 16u;

void main()
{
    // The GS's modulate, clamped as COLCLAMP does.
    vec4 color = min(texture(tex, vUv) * vColor, vec4(1.0));
    // Alpha cut is the GS alpha test GREATER against 0, where alpha 1.0 is
    // 0x80, so the smallest alpha that passes is 1/128.
    if ((pc.flags & kFlagAlphaCut) != 0u && color.a < 1.0 / 128.0)
        discard;
    outColor = color;
}
