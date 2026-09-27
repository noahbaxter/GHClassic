#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUv;

// mvp is Milo's row-vector matrix stored row-major, which GLSL reads as its
// transpose, so mvp * p here is p * M there.
layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 color;
    vec4 params;
} pc;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;

void main()
{
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vColor = inColor * pc.color;
    vUv = inUv;
}
