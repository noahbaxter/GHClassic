#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inNormal;

// Matrices are Milo's row-vector matrices stored row-major, which GLSL reads
// as their transpose, so M * p here is p * M there.
layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 matColor;
    vec4 uvRows; // u's row, then v's
    vec2 uvOffset;
    int boneBase;
    int lightBase;
    uint flags; // colour mode in bits 0-2, prelit 8, alpha cut 16, intensify 32, blended 64
    int envBase;
} pc;

layout(set = 1, binding = 0, std430) readonly buffer FrameData
{
    vec4 data[];
};

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;

const uint kColorVertex = 0u;
const uint kColorAmbient = 1u;
const uint kColorDirectional = 2u;
const uint kColorMaterial = 3u;
const uint kColorPoint = 4u;
const uint kFlagPrelit = 8u;
const uint kFlagIntensify = 32u;
const uint kFlagBlended = 64u;

mat4 bone(int b)
{
    int i = pc.boneBase + b * 4;
    return mat4(data[i], data[i + 1], data[i + 2], data[i + 3]);
}

// The normal as a skin program leaves it. With four bones (0x3c6) it is the
// weighted sum of the normal through each bone's rotation, renormalised
// (ERLENG). With two or three (0x373, 0x394) it goes through the one bone a
// chain of weight compares picks, the heaviest; ties go to the later bone,
// as the two-bone program's does. The batch's program is not kept, so the
// bone count is read off the weights.
vec3 skinNormal(vec4 w)
{
    if (w.w != 0.0)
        return normalize(w.x * mat3(bone(0)) * inNormal + w.y * mat3(bone(1)) * inNormal +
                         w.z * mat3(bone(2)) * inNormal + w.w * mat3(bone(3)) * inNormal);
    int b = w.y >= w.x ? 1 : 0;
    if (w.z != 0.0 && w.z >= max(w.x, w.y))
        b = 2;
    return mat3(bone(b)) * inNormal;
}

void main()
{
    vec4 pos = vec4(inPos, 1.0);
    vec4 vertexColor = inColor;
    // The vert VU1's lighting and tex gen programs see: as loaded, or with
    // two or more bones what the skin program wrote back.
    vec3 litPos = inPos;
    vec3 litNormal = inNormal;
    if (pc.boneBase >= 0)
    {
        // The colour floats are the bone weights, so the vert has no colour
        // of its own.
        pos = inColor.x * (bone(0) * pos) + inColor.y * (bone(1) * pos) + inColor.z * (bone(2) * pos) +
              inColor.w * (bone(3) * pos);
        vertexColor = vec4(1.0);
        if ((pc.flags & kFlagBlended) != 0u)
        {
            litPos = pos.xyz;
            litNormal = skinNormal(inColor);
        }
    }
    gl_Position = pc.mvp * pos;

    // What VU1's lighting program leaves in the vertex's colour.
    uint mode = pc.flags & 7u;
    vec4 base = (pc.flags & kFlagPrelit) != 0u ? vertexColor : pc.matColor;
    vec4 color = vertexColor;
    if (mode == kColorAmbient)
    {
        // 0x7c5: base * ambient, clamped to 1.
        color = min(base * data[pc.lightBase], vec4(1.0));
    }
    else if (mode == kColorMaterial)
    {
        // 0x7c5 without the environ: the material colour, clamped to 1.
        color = min(pc.matColor, vec4(1.0));
    }
    else if (mode == kColorDirectional)
    {
        // 0x6ec: base * ambient + sum of max(0, n . light) * light * material,
        // clamped to 1. Light colours carry alpha 0.
        vec4 ambient = data[pc.lightBase];
        vec3 d = max(vec3(dot(litNormal, data[pc.lightBase + 4].xyz), dot(litNormal, data[pc.lightBase + 5].xyz),
                          dot(litNormal, data[pc.lightBase + 6].xyz)),
                     vec3(0.0));
        vec4 lit = d.x * (data[pc.lightBase + 1] * pc.matColor) + d.y * (data[pc.lightBase + 2] * pc.matColor) +
                   d.z * (data[pc.lightBase + 3] * pc.matColor);
        color = min(lit + base * ambient, vec4(1.0));
    }
    else if (mode == kColorPoint)
    {
        // 0x436: base * ambient, plus within range the light * material *
        // (to . n) * (1/|to| - 1/range), where to runs from the vertex to the
        // light in the mesh's space. The dot is not clamped at zero; the
        // result is clamped to [0, 1].
        vec4 local = data[pc.lightBase + 4];
        vec3 to = local.xyz - litPos;
        float d2 = dot(to, to);
        color = base * data[pc.lightBase];
        if (d2 <= data[pc.lightBase + 5].w && d2 > 0.0)
            color += data[pc.lightBase + 1] * pc.matColor * (dot(to, litNormal) * (local.w + inversesqrt(d2)));
        color = clamp(color, vec4(0.0), vec4(1.0));
    }
    // The GS colour scale: 128 with a texture, raised to 255 by intensify.
    if ((pc.flags & kFlagIntensify) != 0u)
        color.rgb *= 255.0 / 128.0;
    vColor = color;
    if (pc.envBase >= 0)
    {
        // 0x139, the environ tex gen: the vert-to-eye vector reflected about
        // the normal, both taken through the block's rows.
        int e = pc.envBase;
        mat3 wm = mat3(data[e].xyz, data[e + 1].xyz, data[e + 2].xyz); // rows, so wm * x is x . rows
        vec3 v = data[e + 3].xyz - wm * litPos;
        vec3 n = wm * litNormal;
        vec3 r = 2.0 * n * dot(n, v) - v;
        vUv = r.xy * (0.5 * inversesqrt(dot(v, v))) + vec2(0.5);
    }
    else
    {
        vUv = inUv.x * pc.uvRows.xy + inUv.y * pc.uvRows.zw + pc.uvOffset;
    }
}
