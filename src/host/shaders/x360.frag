#version 450

// The 360's standard_ps.fx and lighting.fx for a lit material, lit per
// pixel: normal map in tangent space, Phong specular from a specular map or
// the material's colour, or a cube map in its place, a glow map, a diffuse
// ramp, directional or point lights, and a character's shadow map on the
// first of them. Colour runs 0 to 4, as the 360's 10-bit target holds it
// divided by 4 (pack_color). The environ's fog and fade with distance: of
// 105 environs in the 360's 8 venues one has fog (small2's
// op_Art_projection) and none fades but the track's.

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec3 vWorldPos;
layout(location = 3) in vec3 vNormal;
layout(location = 4) in vec3 vTangent;
layout(location = 5) in vec3 vBitangent;
layout(location = 6) in float vFog;
layout(location = 7) in vec2 vFade;

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
const uint kFlagCutHalf = 1u << 23;
const float kShadowSlopeMost = 8.0;

// shadowfactor.fx's ComputeShadowFactor: how much of the first light reaches
// a character's pixel, from taps of its shadow map two texels about. The 360
// takes eight, turned by an angle from a noise texture a unit of world x and
// y across: the same up a wall, so a shadow's edge is columns of 9 levels, a
// screen door on a face drawn larger than its 720 lines. Here the eight are
// taken at each quarter turn, unturned. Each tap is point sampled from the
// map after downsample_depth.fx has taken the farthest of each texel and its
// eight neighbours, done here at the tap, and passes only where that is
// farther than the pixel. The 360 has no bias; here the pixel's depth
// follows its surface to each tap.
float shadowOf(int b)
{
    vec4 lit = mat4(data[b + 15], data[b + 16], data[b + 17], data[b + 18]) * vec4(vWorldPos, 1.0);
    vec3 at = lit.xyz / lit.w;
    vec2 uv = at.xy * 0.5 + 0.5;
    const vec2 kTaps[8] = vec2[8](vec2(0.000000, 0.000000), vec2(0.527837, -0.085868), vec2(-0.040088, 0.536087),
                                  vec2(-0.670445, -0.179949), vec2(-0.419418, -0.616039),
                                  vec2(0.440453, -0.639399), vec2(-0.757088, 0.349334), vec2(0.574619, 0.685879));
    vec2 turn = vec2(0.0, 1.0) / 256.0;
    ivec2 size = textureSize(shadowMap, 0);
    // The surface's own depth at each tap, by its slope across the map (how
    // the pixel's depth changes with the map's uv, from this pixel's
    // neighbours): a tap to one side of a surface that leans from the light
    // is nearer the light than the pixel by that much, and without it a
    // lit, leaning surface shadows itself in stripes a texel wide. The
    // slope is held to kShadowSlopeMost of the map's depth across its width,
    // past which a surface is edge on to the light and unlit anyway.
    vec2 uvAcross = dFdx(uv), uvDown = dFdy(uv);
    float span = uvAcross.x * uvDown.y - uvAcross.y * uvDown.x;
    vec2 slope = vec2(0.0);
    if (abs(span) > 1e-12)
        slope = vec2(uvDown.y * dFdx(at.z) - uvAcross.y * dFdy(at.z), uvAcross.x * dFdy(at.z) - uvDown.x * dFdx(at.z)) / span;
    float steep = length(slope);
    if (steep > kShadowSlopeMost)
        slope *= kShadowSlopeMost / steep;
    float reached = 0.0;
    for (int quarter = 0; quarter < 4; ++quarter)
    {
        for (int i = 0; i < 8; ++i)
        {
            vec2 tap = uv + kTaps[i].x * turn + kTaps[i].y * vec2(-turn.y, turn.x);
            ivec2 texel = ivec2(floor(tap * vec2(size)));
            float farthest = 1.0;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    farthest = min(farthest, texelFetch(shadowMap, clamp(texel + ivec2(x, y), ivec2(0), size - 1), 0).r);
            // A texel and a half more of slope for the map's own texels.
            float here = at.z + dot(slope, tap - uv) + (abs(slope.x) + abs(slope.y)) * 1.5 / float(size.x);
            reached += farthest < here ? 0.03125 : 0.0;
        }
        turn = vec2(-turn.y, turn.x);
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
    color = mix(data[b + 20], color, vFog);
    // lerp(gFadeColor, output, uv.zzzw), gFadeColor being 0.
    color *= vFade.xxxy;
    color.rgb = clamp(color.rgb, 0.0, 4.0);
    color.a = min(color.a, 1.0);
    // Unblended, its alpha is how many of the pixel's samples it covers, so
    // a thin string still shows as part of a pixel; with one sample, cut at
    // a half.
    int covered = -1;
    if ((pc.flags & kFlagCutHalf) != 0u)
    {
        int count = int(floor(color.a * max(data[b + 6].z, 1.0) + 0.5));
        if (count == 0)
            discard;
        covered = (1 << count) - 1;
        color.a = 1.0;
    }
    gl_SampleMask[0] = covered;
    if ((pc.flags & kFlagAlphaCut) != 0u && color.a < 1.0 / 128.0)
        discard;
    if ((pc.flags & kFlagSetAlpha) != 0u)
        color.a = 1.0;
    outColor = color;
}
