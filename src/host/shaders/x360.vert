#version 450

// The 360's standard_vs.fx for a lit material, per pixel lighting's half:
// world position, normal, tangent and bitangent for x360.frag, and the
// ambient part of the colour (the 360 disc's system/run/shaders).

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inTangent; // w is the bitangent's sign

// As mesh.vert's: matrices are Milo's row-vector matrices stored row-major,
// which GLSL reads as their transpose, so M * p here is p * M there.
layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 matColor;
    vec4 uvRows; // u's row, then v's
    vec2 uvOffset;
    int boneBase;
    int lightBase; // the draw's x360 block (scene_renderer.cpp writeShading)
    uint flags;
    int envBase;
} pc;

layout(set = 1, binding = 0, std430) readonly buffer FrameData
{
    vec4 data[];
};

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec3 vWorldPos;
layout(location = 3) out vec3 vNormal;
layout(location = 4) out vec3 vTangent;
layout(location = 5) out vec3 vBitangent;

const uint kFlagPrelit = 8u;

mat4 rows(int i)
{
    return mat4(data[i], data[i + 1], data[i + 2], data[i + 3]);
}

void main()
{
    vec4 pos = vec4(inPos, 1.0);
    vec4 vertexColor = inColor;
    mat4 world = rows(pc.lightBase);
    vec3 normal = mat3(world) * inNormal;
    vec3 tangent = mat3(world) * inTangent.xyz;
    if (pc.boneBase >= 0)
    {
        // transform.fx's Transform: the weights' sum of the bones, through
        // which position, normal and tangent all go. The colour floats are
        // the weights, so the vert has no colour of its own; the bones hold
        // the world already.
        mat4 trans = inColor.x * rows(pc.boneBase) + inColor.y * rows(pc.boneBase + 4) +
                     inColor.z * rows(pc.boneBase + 8) + inColor.w * rows(pc.boneBase + 12);
        pos = trans * pos;
        normal = mat3(trans) * inNormal;
        tangent = mat3(trans) * inTangent.xyz;
        vertexColor = vec4(1.0);
        world = mat4(1.0);
    }
    gl_Position = pc.mvp * pos;
    vWorldPos = (world * pos).xyz;
    vNormal = normal;
    vTangent = tangent;
    vBitangent = cross(normal, tangent) * inTangent.w;

    // The ambient term: the vertex colour when prelit, else the material
    // colour, times the environ's ambient.
    vec4 base = (pc.flags & kFlagPrelit) != 0u ? vertexColor : pc.matColor;
    vColor = base * vec4(data[pc.lightBase + 5].rgb, 1.0);
    vUv = inUv.x * pc.uvRows.xy + inUv.y * pc.uvRows.zw + pc.uvOffset;
}
