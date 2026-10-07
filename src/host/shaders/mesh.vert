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
    uint flags; // colour mode in bits 0-2, prelit 8, alpha cut 16, intensify 32, skin bones less one in 6-7, projected 256, highlight 512, sphere 1024
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
const uint kFlagVertDyn = 8192u;
const uint kFlagIntensify = 32u;
const uint kSkinBonesShift = 6u;
const uint kFlagProjected = 256u;
const uint kFlagHighlight = 512u;
const uint kFlagSphere = 1024u;

mat4 bone(int b)
{
    int i = pc.boneBase + b * 4;
    return mat4(data[i], data[i + 1], data[i + 2], data[i + 3]);
}

// The normal as the mesh's skin program leaves it.
vec3 skinNormal(vec4 w, uint bones)
{
    // 0x3c6, four bones: the weighted sum through each bone's rotation,
    // renormalised (ERLENG 0x3ed).
    if (bones == 4u)
        return normalize(w.x * mat3(bone(0)) * inNormal + w.y * mat3(bone(1)) * inNormal +
                         w.z * mat3(bone(2)) * inNormal + w.w * mat3(bone(3)) * inNormal);
    // 0x394, three bones: through the heaviest bone alone. The third wins
    // only when strictly heavier (0x3a9..0x3be).
    if (bones == 3u)
    {
        int b = w.y >= w.x ? 1 : 0;
        if (w.z > (b == 1 ? w.y : w.x))
            b = 2;
        return mat3(bone(b)) * inNormal;
    }
    // 0x373, two bones: through the second unless the first is heavier,
    // and then through the first's x and y rows and the second's z row
    // (0x388 adds vf11 where the first's z row is vf7).
    if (w.y >= w.x)
        return mat3(bone(1)) * inNormal;
    return mat3(bone(0))[0] * inNormal.x + mat3(bone(0))[1] * inNormal.y + mat3(bone(1))[2] * inNormal.z;
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
        uint bones = ((pc.flags >> kSkinBonesShift) & 3u) + 1u;
        if (bones > 1u)
        {
            litPos = pos.xyz;
            litNormal = skinNormal(inColor, bones);
        }
    }
    gl_Position = pc.mvp * pos;

    // What VU1's lighting program leaves in the vertex's colour.
    uint mode = pc.flags & 7u;
    vec4 base = (pc.flags & kFlagPrelit) != 0u ? vertexColor : pc.matColor;
    // What a light's colour is scaled by: GH1's vertDyn takes the vertex's.
    vec4 lightScale = (pc.flags & kFlagVertDyn) != 0u ? vertexColor : pc.matColor;
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
        vec4 lit = d.x * (data[pc.lightBase + 1] * lightScale) + d.y * (data[pc.lightBase + 2] * lightScale) +
                   d.z * (data[pc.lightBase + 3] * lightScale);
        color = min(lit + base * ambient, vec4(1.0));
    }
    else if (mode == kColorPoint)
    {
        // 0x436: base * ambient, plus within range the light * material *
        // (to . n) * (1/|to| - 1/range), where to runs from the vertex to the
        // light in the mesh's space. A vert facing away gets none: the
        // light's term is zeroed on the dot's sign flag as it is on the
        // range's (FSAND 0x459, 0x45a). The result is clamped to [0, 1].
        vec4 local = data[pc.lightBase + 4];
        vec3 to = local.xyz - litPos;
        float d2 = dot(to, to);
        float facing = dot(to, litNormal);
        color = base * data[pc.lightBase];
        if (d2 <= data[pc.lightBase + 5].w && d2 > 0.0 && facing >= 0.0)
            color += data[pc.lightBase + 1] * lightScale * (facing * (local.w + inversesqrt(d2)));
        color = clamp(color, vec4(0.0), vec4(1.0));
    }
    // The two environ programs clamp at 1 alone, and FTOI0's negative
    // integer reaches the GS as its low byte: -11 is 245, nearly twice white.
    if (mode == kColorAmbient || mode == kColorDirectional)
    {
        float scale = (pc.flags & kFlagIntensify) != 0u ? 255.0 : 128.0;
        vec3 units = color.rgb * scale;
        color.rgb = mix(color.rgb, mod(trunc(units), 256.0) / scale, lessThan(units, vec3(0.0)));
    }
    // The GS colour scale: 128 with a texture, raised to 255 by intensify.
    if ((pc.flags & kFlagIntensify) != 0u)
        color.rgb *= 255.0 / 128.0;
    // A HIGHLIGHT material's alpha is 0.035 whatever its colour's: Select
    // writes it over qw690's (0x3d8614), MakeRGBAQ over a rect's (0x199164).
    if ((pc.flags & kFlagHighlight) != 0u)
        color.a = 0.035;
    vColor = color;
    if (pc.envBase >= 0 && (pc.flags & kFlagSphere) != 0u)
    {
        // 0x347, the sphere tex gen: the normal through the block's rows,
        // plus its offset.
        int e = pc.envBase;
        mat3 wm = mat3(data[e].xyz, data[e + 1].xyz, data[e + 2].xyz);
        vUv = (wm * litNormal).xy + data[e + 3].xy;
    }
    else if (pc.envBase >= 0 && (pc.flags & kFlagProjected) != 0u)
    {
        // 0x410, the projected tex gen: the vert through the block's rows
        // and offset, which take it to the projector's space. No divide.
        int e = pc.envBase;
        mat3 wm = mat3(data[e].xyz, data[e + 1].xyz, data[e + 2].xyz);
        vUv = (wm * litPos + data[e + 3].xyz).xy;
    }
    else if (pc.envBase >= 0)
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
