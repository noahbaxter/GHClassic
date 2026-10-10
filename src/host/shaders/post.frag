#version 450

// The 360's post stage for the world, one step a pass (the 360 disc's
// system/run/shaders, constants from a Xenia frame of battle:
// .planning/gh2x-look/xenia/frame-summary.md).
//   0 bloom.fx: what is bright enough to bloom, from the scene
//   1 downsample.fx: the mean of a 2x2 block
//   2 blur.fx: 15 taps along `step`, weights summing to 2.47
//   3 postprocess.fx: the scene, its bloom added, through the luminance map
//   4 the finished frame through the display's gamma ramp

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
// The ramp the game gives the 360's DAC, the same for red, green and blue:
// what each 8-bit level of the finished frame goes out as, of 1023. Read out
// of the Xenia frame; no one power curve.
const float kRamp[256] = float[256](
    0, 4, 7, 10, 13, 15, 18, 21, 23, 27, 30, 33, 37, 41, 45, 48, 52, 56, 61, 65,
    71, 75, 80, 85, 91, 96, 101, 106, 111, 116, 121, 126, 131, 135, 140, 145, 150, 154, 159, 164,
    168, 173, 178, 182, 187, 191, 196, 200, 206, 210, 215, 219, 224, 228, 232, 237, 241, 246, 251, 255,
    259, 264, 268, 272, 277, 282, 286, 290, 294, 298, 303, 308, 312, 316, 320, 325, 329, 333, 337, 341,
    346, 350, 354, 358, 362, 367, 371, 375, 379, 383, 388, 392, 396, 400, 405, 409, 413, 417, 421, 425,
    429, 433, 438, 442, 446, 449, 454, 458, 462, 466, 470, 474, 478, 482, 487, 490, 494, 499, 503, 506,
    510, 515, 518, 522, 527, 531, 534, 538, 543, 546, 550, 555, 558, 562, 567, 570, 574, 577, 582, 586,
    589, 594, 598, 601, 606, 609, 613, 616, 621, 625, 628, 633, 636, 640, 644, 648, 651, 656, 660, 663,
    668, 671, 675, 679, 683, 686, 691, 694, 698, 702, 706, 709, 713, 717, 720, 725, 728, 732, 736, 740,
    743, 747, 751, 754, 759, 762, 767, 770, 773, 778, 781, 785, 789, 792, 796, 800, 804, 808, 811, 815,
    819, 822, 826, 830, 833, 837, 841, 844, 849, 852, 855, 860, 863, 866, 871, 874, 877, 882, 885, 889,
    893, 896, 900, 903, 908, 911, 914, 919, 922, 925, 929, 933, 937, 940, 943, 948, 951, 955, 958, 962,
    966, 969, 972, 977, 980, 984, 987, 992, 995, 998, 1002, 1005, 1010, 1013, 1016, 1020);

// The ramp between two levels, since the frame here is not yet 8-bit.
float ramp(float level)
{
    float at = clamp(level, 0.0, 1.0) * 255.0;
    int below = min(int(at), 254);
    return mix(kRamp[below], kRamp[below + 1], at - float(below)) / 1023.0;
}

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
    else if (pc.mode == 4u)
    {
        vec4 colour = texelFetch(source, ivec2(gl_FragCoord.xy), 0); // the same size
        outColor = vec4(ramp(colour.r), ramp(colour.g), ramp(colour.b), clamp(colour.a, 0.0, 1.0));
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
