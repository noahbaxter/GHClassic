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
    int textBase;
} pc;

// The vector fonts (render/vector_fonts.h): where the pack's glyph layers,
// bands and lines start, then those, then a table per font a text draws with.
layout(set = 1, binding = 1, std430) readonly buffer VectorFonts
{
    uint vf[];
};

layout(location = 0) out vec4 outColor;

const uint kFlagAlphaCut = 16u;
const uint kFlagHighlight = 512u;
const uint kFlagSpread = 2048u;
const uint kFlagSetAlpha = 4096u;
const uint kFlagDecal = 16384u;
const uint kFlagAdd16 = 32768u;
const uint kFlagCutHalf = 1u << 23;

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

float vff(uint i)
{
    return uintBitsToFloat(vf[i]);
}

// A line's two ends, packed as int16 pairs in 1/32 pixels (fontpack.py).
vec4 vectorLine(uint i)
{
    uint a = vf[i], b = vf[i + 1u];
    return vec4(ivec4(int(a << 16) >> 16, int(a) >> 16, int(b << 16) >> 16, int(b) >> 16)) / 32.0;
}

// How much of a pixel at p, in the pack's pixels, glyph layer g covers, the
// pixel spanning `size` of them: inside by the crossings of a ray to +x, the
// edge softened over the pixel by the distance to the nearest line. And that
// distance, signed, positive inside, measured out to `reach` past the edge.
vec2 vectorCoverage(uint g, vec2 p, float size, float reach)
{
    uint at = vf[0] + g * 8u;
    vec4 bounds = vec4(vff(at), vff(at + 1u), vff(at + 2u), vff(at + 3u));
    reach = max(reach, size);
    if (any(lessThan(p, bounds.xy - reach)) || any(greaterThan(p, bounds.zw + reach)))
        return vec2(0.0, -reach);
    float bandH = vff(at + 5u);
    uint bands = vf[at + 7u];
    uint band = vf[at + 6u] + uint(clamp(int(floor((p.y - vff(at + 4u)) / bandH)), 0, int(bands) - 1));
    uint first = vf[vf[1] + band * 2u], count = vf[vf[1] + band * 2u + 1u];
    bool inside = false;
    float nearest = 1e9;
    for (uint i = 0u; i < count; ++i)
    {
        vec4 l = vectorLine(vf[2] + (first + i) * 2u);
        vec2 a = l.xy, b = l.zw, ab = b - a;
        if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (p.y - a.y) * ab.x / ab.y)
            inside = !inside;
        float t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-8), 0.0, 1.0);
        vec2 d = p - (a + t * ab);
        nearest = min(nearest, dot(d, d));
    }
    float distance = sqrt(nearest) * (inside ? 1.0 : -1.0);
    return vec2(clamp(distance / size + 0.5, 0.0, 1.0), distance);
}

// The texel a vector font's glyph gives at uv: its layers over each other in
// their colours, coverage as alpha. A masked font's is the texture's own,
// cut to the outline, with the font's tone standing in for the texture's
// fade over its edge's width of texels inside the outline (none at 0), and
// the texture's own fade kept past it, dying out over its feather's texels.
// The texture's own where the font's table has no glyph for the quad.
vec4 vectorTexel(vec2 uv, vec2 dx, vec2 dy)
{
    uint base = uint(pc.textBase);
    uint count = vf[base], layers = vf[base + 1u];
    vec2 scale = vec2(vff(base + 2u), vff(base + 3u));
    for (uint i = 0u; i < count; ++i)
    {
        uint e = base + 8u + i * 8u;
        vec4 rect = vec4(vff(e), vff(e + 1u), vff(e + 2u), vff(e + 3u));
        if (uv.x < rect.x || uv.y < rect.y || uv.x > rect.z || uv.y > rect.w)
            continue;
        // The pack's pixels put a pixel's center on its index.
        vec2 p = (uv - vec2(vff(e + 4u), vff(e + 5u))) * scale - 0.5;
        float size = max(0.5 * (length(dx * scale) + length(dy * scale)), 1e-3);
        if ((vf[base + 6u] & 1u) != 0u && vf[e + 6u] != 0xffffffffu)
        {
            // The pack's pixels to the texture's texels.
            float perTexel = float(textureSize(tex, 0).x) / scale.x;
            float edge = float((vf[base + 6u] >> 8u) & 0xffu) * 0.25;
            float feather = float((vf[base + 6u] >> 16u) & 0xffu) * 0.25;
            vec2 cover = vectorCoverage(vf[e + 6u], p, size, feather / perTexel);
            vec4 texel = textureGrad(tex, uv, dx, dy);
            float texels = cover.y * perTexel;
            float body = edge > 0.0 ? mix(vff(base + 7u), texel.a, smoothstep(0.25 * edge, edge, texels)) : texel.a;
            float alpha = feather > 0.0 ? max(cover.x, clamp(1.0 + texels / feather, 0.0, 1.0)) : cover.x;
            return vec4(texel.rgb, alpha * body);
        }
        vec4 color = vec4(0.0);
        for (uint l = 0u; l < layers; ++l)
        {
            uint g = vf[e + 6u + l];
            if (g == 0xffffffffu)
                continue;
            float a = vectorCoverage(g, p, size, size).x;
            vec3 rgb = unpackUnorm4x8(vf[base + 4u + l]).rgb;
            float over = a + color.a * (1.0 - a);
            color.rgb = over > 0.0 ? (rgb * a + color.rgb * color.a * (1.0 - a)) / over : rgb;
            color.a = over;
        }
        return color;
    }
    return textureGrad(tex, uv, dx, dy);
}

void main()
{
    // Taken here, where every pixel of the quad runs it: a vector glyph's
    // texel is found in a loop, where derivatives are undefined.
    vec2 dx = dFdx(vUv), dy = dFdy(vUv);
    // The GS's modulate, clamped as COLCLAMP does.
    vec4 texel = pc.textBase >= 0 ? vectorTexel(vUv, dx, dy) : texture(tex, vUv);
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
    if ((pc.flags & kFlagAlphaCut) != 0u && color.a < ((pc.flags & kFlagCutHalf) != 0u ? 0.5 : 1.0 / 128.0))
        discard;
    if ((pc.flags & kFlagSetAlpha) != 0u)
        color.a = 1.0;
    outColor = color;
}
