#version 450

// The 360's post stage for the world, one step a pass (the 360 disc's
// system/run/shaders, constants from a Xenia frame of battle:
// .planning/gh2x-look/xenia/frame-summary.md).
//   0 bloom.fx: what is bright enough to bloom, from the scene
//   1 downsample.fx: the mean of a 2x2 block
//   2 blur.fx: 15 taps along `step`, weights summing to 2.47
//   3 postprocess.fx: the scene, its bloom added, through the luminance map

layout(location = 0) in vec2 vUv;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D luminanceMap;

layout(push_constant) uniform Push
{
    vec4 params; // xy: a step in uv (2: the blur's, 1: one source texel); z: bloom intensity; w: 1 with a luminance map
    uint mode;
} pc;

layout(location = 0) out vec4 outColor;

const vec3 kLuminance = vec3(0.2125, 0.7154, 0.0721);
// gBloomThresholds, the same in every PostProc of the disc.
const vec4 kThresholds = vec4(3.9, 3.9, 3.9, 1.0);
const float kWeights[8] = float[8](0.33245, 0.31449, 0.26621, 0.20164, 0.13668, 0.08290, 0.04499, 0.02185);

void main()
{
    if (pc.mode == 0u)
    {
        // Colour past the thresholds, and white bloom in alpha, by
        // luminance from 1 to 4, which the colour part gives way to.
        vec3 tap = texelFetch(source, ivec2(gl_FragCoord.xy), 0).rgb; // the same size, point sampled
        float white = smoothstep(kThresholds.a, 4.0, dot(tap, kLuminance));
        vec3 colour = clamp((tap - kThresholds.rgb) * 1024.0, 0.0, 1.0);
        outColor = vec4(clamp(colour - white, 0.0, 1.0), white);
    }
    else if (pc.mode == 1u)
    {
        vec2 h = 0.5 * pc.params.xy;
        outColor = 0.25 * (texture(source, vUv + vec2(-h.x, -h.y)) + texture(source, vUv + vec2(h.x, -h.y)) +
                           texture(source, vUv + vec2(-h.x, h.y)) + texture(source, vUv + vec2(h.x, h.y)));
    }
    else if (pc.mode == 2u)
    {
        vec4 sum = kWeights[0] * texture(source, vUv);
        for (int i = 1; i < 8; ++i)
            sum += kWeights[i] * (texture(source, vUv + float(i) * pc.params.xy) +
                                  texture(source, vUv - float(i) * pc.params.xy));
        outColor = sum;
    }
    else
    {
        vec4 colour = texture(source, vUv);
        vec4 glow = texture(bloom, vUv);
        glow.rgb += glow.a;
        colour += glow * pc.params.z;
        if (pc.params.w != 0.0)
        {
            // The map is indexed by luminance over 4 and scales the colour;
            // past 1 it is scaled down to keep white white.
            float lum = dot(colour.rgb, kLuminance) / 4.0;
            float scale = textureLod(luminanceMap, vec2(lum, 0.0), 0.0).r;
            if (lum > 1.0)
                scale /= lum;
            colour.rgb *= scale;
        }
        outColor = colour;
    }
}
