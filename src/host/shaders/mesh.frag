#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) noperspective in float vFog;
layout(location = 3) flat in vec3 vFogColor;

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
    int fogBase;
} pc;

layout(location = 0) out vec4 outColor;

const uint kFlagAlphaCut = 16u;
const uint kFlagHighlight = 512u;
const uint kFlagSpread = 2048u;
const uint kFlagSetAlpha = 4096u;
const uint kFlagDecal = 16384u;
const uint kFlagAdd16 = 32768u;

// Added to a 16-bit frame under DIMX's 0 to 3 (PsMat::Update, 0x19d0a4), a
// colour keeps its whole eights and of the rest what the dither carries
// over: none of 4 or less, a quarter of the pixels at 5, half at 6, three
// quarters at 7.
vec3 added16(vec3 rgb)
{
    vec3 c = min(rgb, vec3(1.0)) * 255.0;
    vec3 whole = floor(c / 8.0) * 8.0;
    return (whole + max(c - whole - 4.0, vec3(0.0)) * 2.0) / 255.0;
}

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
    // TFX DECAL: the texel alone.
    vec4 color = (pc.flags & kFlagDecal) != 0u ? texel : texel * vColor;
    if ((pc.flags & kFlagHighlight) != 0u)
    {
        // TFX HIGHLIGHT: Ct * Cf + Af, alpha At + Af. Af is the vertex
        // alpha as VU1's FTOI0 leaves it, added to rgb in 255ths.
        float af = floor(vColor.a * 128.0);
        color = vec4(color.rgb + af / 255.0, texel.a + af / 128.0);
        color.rgb = added16(color.rgb);
    }
    if ((pc.flags & kFlagAdd16) != 0u)
        color.rgb = added16(color.rgb);
    color = min(color, vec4(1.0));
    // The GS's fog, per pixel by the interpolated F, before the blend: F of
    // 255 leaves the colour, 0 is the fog's alone.
    if (pc.fogBase >= 0)
        color.rgb = mix(vFogColor, color.rgb, vFog);
    // Alpha cut is the GS alpha test GREATER against 0, where alpha 1.0 is
    // 0x80, so the smallest alpha that passes is 1/128.
    if ((pc.flags & kFlagAlphaCut) != 0u && color.a < 1.0 / 128.0)
        discard;
    if ((pc.flags & kFlagSetAlpha) != 0u)
        color.a = 1.0;
    outColor = color;
}
