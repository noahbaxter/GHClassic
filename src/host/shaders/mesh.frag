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
    int envBase;
} pc;

layout(location = 0) out vec4 outColor;

const uint kFlagAlphaCut = 16u;
const uint kFlagHighlight = 512u;
const uint kFlagSpread = 2048u;

void main()
{
    // The GS's modulate, clamped as COLCLAMP does.
    vec4 texel = texture(tex, vUv);
    if ((pc.flags & kFlagSpread) != 0u)
    {
        vec2 h = 0.5 * pc.matColor.xy;
        texel = 0.25 * (texture(tex, vUv + vec2(-h.x, -h.y)) + texture(tex, vUv + vec2(h.x, -h.y)) +
                        texture(tex, vUv + vec2(-h.x, h.y)) + texture(tex, vUv + vec2(h.x, h.y)));
    }
    vec4 color = texel * vColor;
    if ((pc.flags & kFlagHighlight) != 0u)
    {
        // TFX HIGHLIGHT: Ct * Cf + Af, alpha At + Af. Af is the vertex
        // alpha as VU1's FTOI0 leaves it, added to rgb in 255ths.
        float af = floor(vColor.a * 128.0);
        color = vec4(color.rgb + af / 255.0, texel.a + af / 128.0);
    }
    color = min(color, vec4(1.0));
    // Alpha cut is the GS alpha test GREATER against 0, where alpha 1.0 is
    // 0x80, so the smallest alpha that passes is 1/128.
    if ((pc.flags & kFlagAlphaCut) != 0u && color.a < 1.0 / 128.0)
        discard;
    outColor = color;
}
