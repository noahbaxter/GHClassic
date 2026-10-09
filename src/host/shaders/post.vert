#version 450

// One triangle over the whole target, uv 0 to 1 across what it covers.

layout(location = 0) out vec2 vUv;

void main()
{
    vec2 corner = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
