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
    uint flags; // colour mode in bits 0-1, prelit 4, alpha cut 8, intensify 16
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
const uint kFlagPrelit = 4u;
const uint kFlagIntensify = 16u;

mat4 bone(int b)
{
    int i = pc.boneBase + b * 4;
    return mat4(data[i], data[i + 1], data[i + 2], data[i + 3]);
}

void main()
{
    vec4 pos = vec4(inPos, 1.0);
    vec4 vertexColor = inColor;
    if (pc.boneBase >= 0)
    {
        // The colour floats are the bone weights, so the vert has no colour
        // of its own.
        pos = inColor.x * (bone(0) * pos) + inColor.y * (bone(1) * pos) + inColor.z * (bone(2) * pos) +
              inColor.w * (bone(3) * pos);
        vertexColor = vec4(1.0);
    }
    gl_Position = pc.mvp * pos;

    // What VU1's lighting program leaves in the vertex's colour.
    uint mode = pc.flags & 3u;
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
        vec3 d = max(vec3(dot(inNormal, data[pc.lightBase + 4].xyz), dot(inNormal, data[pc.lightBase + 5].xyz),
                          dot(inNormal, data[pc.lightBase + 6].xyz)),
                     vec3(0.0));
        vec4 lit = d.x * (data[pc.lightBase + 1] * pc.matColor) + d.y * (data[pc.lightBase + 2] * pc.matColor) +
                   d.z * (data[pc.lightBase + 3] * pc.matColor);
        color = min(lit + base * ambient, vec4(1.0));
    }
    // The GS colour scale: 128 with a texture, raised to 255 by intensify.
    if ((pc.flags & kFlagIntensify) != 0u)
        color.rgb *= 255.0 / 128.0;
    vColor = color;
    vUv = inUv.x * pc.uvRows.xy + inUv.y * pc.uvRows.zw + pc.uvOffset;
}
