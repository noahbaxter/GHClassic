#version 450

// A mesh that moved since the last frame, as it is now and as it was then,
// for motion.frag: a vert goes through the draw's matrix and bones as
// mesh.vert and x360.vert take it, and through last frame's.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor; // the bone weights when skinned

// mesh.vert's block, with envBase standing for where last frame's matrix
// (four vec4s) and then its four bones lie in the frame data.
layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 matColor;
    vec4 uvRows;
    vec2 uvOffset;
    int boneBase;
    int lightBase;
    uint flags;
    int wasBase;
    int fogBase;
} pc;

layout(set = 1, binding = 0, std430) readonly buffer FrameData
{
    vec4 data[];
};

layout(location = 0) out vec4 vNow;
layout(location = 1) out vec4 vWas;

mat4 rows(int i)
{
    return mat4(data[i], data[i + 1], data[i + 2], data[i + 3]);
}

mat4 skin(int base)
{
    return inColor.x * rows(base) + inColor.y * rows(base + 4) + inColor.z * rows(base + 8) +
           inColor.w * rows(base + 12);
}

void main()
{
    vec4 pos = vec4(inPos, 1.0);
    vec4 was = pos;
    if (pc.boneBase >= 0)
    {
        pos = skin(pc.boneBase) * pos;
        was = skin(pc.wasBase + 4) * was;
    }
    gl_Position = pc.mvp * pos;
    vNow = gl_Position;
    vWas = rows(pc.wasBase) * was;
}
