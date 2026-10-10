#version 450

// The 360's standard_ps.fx and lighting.fx for a lit material, lit per
// pixel: normal map in tangent space, Phong specular from a specular map or
// the material's colour, or a cube map in its place, a glow map, a diffuse
// ramp, directional or point lights, and a character's shadow map on the
// first of them. Colour runs 0 to 4, as the 360's 10-bit target holds it
// divided by 4 (pack_color). No fog or fade.

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in vec3 vNormal;
layout(location = 4) in vec3 vTangent;
layout(location = 5) in vec3 vBitangent;

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;
layout(set = 2, binding = 0) uniform sampler2D normalMap;
layout(set = 2, binding = 1) uniform sampler2D specularMap;
layout(set = 2, binding = 2) uniform sampler2D glowMap;
layout(set = 2, binding = 3) uniform sampler2D rampMap;
layout(set = 2, binding = 4) uniform samplerCube environMap;
layout(set = 3, binding = 0) uniform sampler2D shadowMap; // depth from the light, nearer is larger

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

layout(set = 1, binding = 0, std430) readonly buffer FrameData
{
    vec4 data[];
};

layout(location = 0) out vec4 outColor;

const uint kFlagAlphaCut = 16u;
const uint kFlagSetAlpha = 4096u;
const uint kFlagNormalMap = 1u << 16;
const uint kFlagSpecularMap = 1u << 17;
const uint kFlagGlowMap = 1u << 18;
const uint kFlagRamp = 1u << 19;
const uint kFlagCube = 1u << 20;
const uint kFlagNormalize = 1u << 21;
const uint kFlagShadow = 1u << 22;

// One texel of a 32x32 noise texture that repeats.
float noiseOf(vec2 texel)
{
    return fract(sin(dot(mod(texel, 32.0), vec2(12.9898, 78.233))) * 43758.5453);
}

// shadowfactor.fx's ComputeShadowFactor: how much of the first light reaches
// a character's pixel, from eight taps of its shadow map two texels about,
// turned by an angle that changes over the world. The 360 reads that angle
// from a 32x32 noise texture a unit of world x and y across, filtered when
// magnified, which is not on the disc; a hash stands in. Each tap is point
// sampled from the map after downsample_depth.fx has taken the farthest of
// each texel and its eight neighbours, done here at the tap, and passes only
// where that is farther than the pixel. No bias.
float shadowOf(int b)
{
    vec4 lit = mat4(data[b + 15], data[b + 16], data[b + 17], data[b + 18]) * vec4(vWorldPos, 1.0);
    vec3 at = lit.xyz / lit.w;
    vec2 uv = at.xy * 0.5 + 0.5;
    const vec2 kTaps[8] = vec2[8](vec2(0.000000, 0.000000), vec2(0.527837, -0.085868), vec2(-0.040088, 0.536087),
                                  vec2(-0.670445, -0.179949), vec2(-0.419418, -0.616039),
                                  vec2(0.440453, -0.639399), vec2(-0.757088, 0.349334), vec2(0.574619, 0.685879));
    vec2 cell = vWorldPos.xy * 32.0 - 0.5;
    vec2 part = fract(cell);
    float noise = mix(mix(noiseOf(floor(cell)), noiseOf(floor(cell) + vec2(1.0, 0.0)), part.x),
                      mix(noiseOf(floor(cell) + vec2(0.0, 1.0)), noiseOf(floor(cell) + vec2(1.0, 1.0)), part.x), part.y);
    float angle = (noise * 2.0 - 1.0) * 3.14159;
    vec2 turn = vec2(sin(angle), cos(angle)) / 256.0;
    ivec2 size = textureSize(shadowMap, 0);
    float reached = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        vec2 tap = uv + kTaps[i].x * turn + kTaps[i].y * vec2(-turn.y, turn.x);
        ivec2 texel = ivec2(floor(tap * vec2(size)));
        float farthest = 1.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                farthest = min(farthest, texelFetch(shadowMap, clamp(texel + ivec2(x, y), ivec2(0), size - 1), 0).r);
        reached += farthest < at.z ? 0.125 : 0.0;
    }
    return reached;
}

// lighting.fx's Diffuse: n . l, or the ramp looked up by it.
vec3 diffuseOf(vec3 normal, vec3 toLight)
{
    if ((pc.flags & kFlagRamp) != 0u)
        return textureLod(rampMap, vec2(0.5 * dot(normal, toLight) + 0.5, 0.0), 0.0).rgb;
    return vec3(clamp(dot(normal, toLight), 0.0, 1.0));
}

// lighting.fx's Specular.
float specularOf(vec3 normal, vec3 toLight, vec3 toEye, float power)
{
    return pow(clamp(dot(reflect(-toLight, normal), toEye), 0.0, 1.0), max(power, 1e-4));
}

void main()
{
    int b = pc.lightBase;
    bool cube = (pc.flags & kFlagCube) != 0u;

    // The world normal, through the normal map's tangent space. The 360
    // reads the map's x and y as the normal's y and x; its x is ATI2's
    // first block, which formats/dxt.cpp decodes to green, so here x is
    // red and y green (checked against what Xenia sampled for the Les Paul).
    // Its z is made cheaply as the 360 does, and it is turned with the uv
    // transform.
    vec3 normal = vNormal;
    if ((pc.flags & kFlagNormalMap) != 0u)
    {
        vec3 n = vNormal;
        vec3 t = vTangent;
        vec3 bt = vBitangent;
        if ((pc.flags & kFlagNormalize) != 0u)
        {
            n = normalize(n);
            t = normalize(t);
            bt = normalize(bt);
        }
        vec3 local = vec3(2.0 * texture(normalMap, vUv).xy - 1.0, 0.0);
        local.z = clamp(1.35 - abs(local.x) - abs(local.y), 0.0, 1.0);
        local.xy = local.x * pc.uvRows.xy + local.y * pc.uvRows.zw;
        normal = local.x * t + local.y * bt + local.z * n;
    }
    vec3 toEye = normalize(data[b + 4].xyz - vWorldPos);
    normal = normalize(normal);

    // rgb masks the specular, alpha is its power (or the cube's blur).
    vec4 specFactor = data[b + 13];
    if ((pc.flags & kFlagSpecularMap) != 0u)
    {
        specFactor = texture(specularMap, vUv);
        specFactor.a = max(specFactor.a * data[b + 13].a, 0.1);
    }

    vec3 emissive = (pc.flags & kFlagGlowMap) != 0u ? texture(glowMap, vUv).rgb : vec3(0.0);

    // ApplyLights: directional lights, or point lights each fading to
    // nothing at its range.
    vec3 lit = vec3(0.0);
    vec3 specular = vec3(0.0);
    int count = int(data[b + 6].x);
    bool point = data[b + 6].y != 0.0;
    for (int i = 0; i < count; ++i)
    {
        vec3 color = data[b + 10 + i].rgb;
        // ShadowPoint: the shadow map cuts the first light, diffuse and
        // specular alike.
        if (i == 0 && (pc.flags & kFlagShadow) != 0u)
            color *= shadowOf(b);
        vec4 light = data[b + 7 + i];
        if (point)
        {
            vec3 to = light.xyz - vWorldPos;
            float len = length(to);
            to /= max(len, 1e-20);
            float near = 1.0 - clamp(len * light.w, 0.0, 1.0);
            lit += color * diffuseOf(normal, to).x * near;
            if (!cube)
                specular += color * specularOf(normal, to, toEye, specFactor.a) * near;
        }
        else
        {
            lit += color * diffuseOf(normal, light.xyz);
            if (!cube)
                specular += color * specularOf(normal, light.xyz, toEye, specFactor.a);
        }
    }
    // The material colour goes on the lights twice, as written: in
    // DirLights or PointLights, then in the pixel shader.
    vec4 diffuse = vec4(lit * pc.matColor.rgb, 0.0) * pc.matColor;
    specular *= specFactor.rgb;

    diffuse += vColor;
    diffuse *= texture(diffuseMap, vUv) * data[b + 14].y;

    if (cube)
    {
        // The reflection with y and z swapped, Milo's z up to D3D's y up,
        // its blur the specular power.
        vec3 r = reflect(-toEye, normal);
        specular = texture(environMap, r.xzy, specFactor.a).rgb * specFactor.rgb;
    }

    vec4 color = vec4(emissive * data[b + 14].x + diffuse.rgb + specular, diffuse.a);
    color.rgb = clamp(color.rgb, 0.0, 4.0);
    color.a = min(color.a, 1.0);
    if ((pc.flags & kFlagAlphaCut) != 0u && color.a < 1.0 / 128.0)
        discard;
    if ((pc.flags & kFlagSetAlpha) != 0u)
        color.a = 1.0;
    outColor = color;
}
