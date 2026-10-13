#version 450

// The 360's post stage for the world, one step a pass (the 360 disc's
// system/run/shaders, constants from a Xenia frame of battle:
// .planning/gh2x-look/xenia/frame-summary.md).
//   0 bloom.fx: what is bright enough to bloom, from the scene
//   1 downsample.fx: the mean of a 2x2 block
//   2 blur.fx: 15 taps along `step`, weights summing to 2.47
//   3 postprocess.fx: the scene, blurred by depth of field, its bloom added,
//     through the luminance map
//   4 the finished frame through the display's gamma ramp
//   5 downsample_collapse.fx: the scene's low picture for depth of field
//   6 blur.fx: 7 equal taps along `step`
//   7 the multisampled scene into one sample a pixel (ours, not the 360's)
//   8 the scene held steady over frames (ours): each frame is drawn a part
//     of a pixel off the last, and joins what the frames before it left
// Built a second time with MULTISAMPLED for the last world pass (3) over a
// multisampled depth buffer, and for 7 and 8.

layout(location = 0) in vec2 vUv;

layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1) uniform sampler2D bloom;
layout(set = 0, binding = 2) uniform sampler2D luminanceMap;
layout(set = 0, binding = 3) uniform sampler2D blurred;
#ifdef MULTISAMPLED
layout(set = 0, binding = 4) uniform sampler2DMS depth;
#else
layout(set = 0, binding = 4) uniform sampler2D depth;
#endif

layout(push_constant) uniform Push
{
    // xy: a step in uv (2, 6: the blur's; 1, 5: one source texel);
    // z: bloom intensity (8: 1 with frames before this one to join);
    // w: 1 with a luminance map
    vec4 params;
    // Depth of field: the blur is the depth buffer's value times x plus y,
    // either side of 0, held between z and w. w is 0 for none.
    vec4 focus;
    uint mode;
    // 3: how much of its edge a pixel is given back, 0 for none
    float sharpen;
    // 8: this frame's clip space to the last frame's
    mat4 reproject;
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

// A colour brought under 1 by its luminance, so a mean is not ruled by its
// brightest part, and back.
vec3 held(vec3 colour)
{
    return colour / (1.0 + dot(colour, kLuminance));
}

vec3 freed(vec3 colour)
{
    return colour / max(1.0 - dot(colour, kLuminance), 1.0e-4);
}

// Catmull-Rom over 4x4 texels in nine linear taps: a plain linear tap of the
// frames before softens them a little more every frame the camera moves.
vec3 sharp(sampler2D image, vec2 uv)
{
    vec2 size = vec2(textureSize(image, 0));
    vec2 at = uv * size;
    vec2 first = floor(at - 0.5) + 0.5;
    vec2 f = at - first;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (first - 1.0) / size;
    vec2 t12 = (first + w2 / w12) / size;
    vec2 t3 = (first + 2.0) / size;
    vec3 sum = textureLod(image, vec2(t0.x, t0.y), 0.0).rgb * w0.x * w0.y +
               textureLod(image, vec2(t12.x, t0.y), 0.0).rgb * w12.x * w0.y +
               textureLod(image, vec2(t3.x, t0.y), 0.0).rgb * w3.x * w0.y +
               textureLod(image, vec2(t0.x, t12.y), 0.0).rgb * w0.x * w12.y +
               textureLod(image, vec2(t12.x, t12.y), 0.0).rgb * w12.x * w12.y +
               textureLod(image, vec2(t3.x, t12.y), 0.0).rgb * w3.x * w12.y +
               textureLod(image, vec2(t0.x, t3.y), 0.0).rgb * w0.x * w3.y +
               textureLod(image, vec2(t12.x, t3.y), 0.0).rgb * w12.x * w3.y +
               textureLod(image, vec2(t3.x, t3.y), 0.0).rgb * w3.x * w3.y;
    return max(sum, vec3(0.0));
}

void main()
{
    if (pc.mode == 8u)
    {
        // `bloom` is the frames before this one. Where this pixel's surface
        // was in them comes from its depth and the two cameras, so what
        // moves on its own (a player, a light's beam) is found in the wrong
        // place: what is found is held to the colours around the pixel now,
        // which keeps a wrong find from showing as a trail.
        ivec2 at = ivec2(gl_FragCoord.xy);
        ivec2 size = textureSize(source, 0);
        vec4 now = texelFetch(source, at, 0);
        outColor = now;
        if (pc.params.z == 0.0)
            return;
        // The nearest depth of the nine (greater is nearer), so an edge
        // moves with what is in front.
        float z = 0.0;
        vec3 sum = vec3(0.0), squares = vec3(0.0);
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                ivec2 tap = clamp(at + ivec2(x, y), ivec2(0), size - 1);
                z = max(z, texelFetch(depth, tap, 0).r);
                vec3 colour = held(texelFetch(source, tap, 0).rgb);
                sum += colour;
                squares += colour * colour;
            }
        // From the pixel's middle, not from where this frame's shift put
        // what it shows: a still camera then finds each pixel in itself.
        vec4 was = pc.reproject * vec4(gl_FragCoord.xy / vec2(size) * 2.0 - 1.0, z, 1.0);
        vec2 uv = was.xy / was.w * 0.5 + 0.5;
        if (was.w <= 0.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
            return;
        vec3 mean = sum / 9.0;
        vec3 reach = sqrt(max(squares / 9.0 - mean * mean, vec3(0.0))) + 1.0e-5;
        vec3 off = held(sharp(bloom, uv)) - mean;
        vec3 over = abs(off) / reach;
        off /= max(1.0, max(over.r, max(over.g, over.b)));
        outColor.rgb = freed(mix(mean + off, held(now.rgb), 0.1));
    }
    else if (pc.mode == 0u)
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
    else if (pc.mode == 5u)
    {
        // Four of the scene's texels, half of one each way from here, point
        // sampled. The target holds 0 to 1, so the low picture has no more.
        ivec2 size = textureSize(source, 0);
        vec2 at = vUv * vec2(size);
        vec3 sum = vec3(0.0);
        for (int i = 0; i < 4; ++i)
        {
            vec2 corner = vec2(float(i & 1), float(i >> 1)) - 0.5;
            sum += texelFetch(source, clamp(ivec2(floor(at + corner)), ivec2(0), size - 1), 0).rgb;
        }
        outColor = vec4(0.25 * sum, 0.0);
    }
    else if (pc.mode == 6u)
    {
        // At 0, 1, 1 2/3 and 2 1/3 of the 360's 640x320 texels.
        const float kTaps[3] = float[3](1.0, 5.0 / 3.0, 7.0 / 3.0);
        vec4 sum = texture(source, vUv);
        for (int i = 0; i < 3; ++i)
            sum += texture(source, vUv + kTaps[i] * pc.params.xy) + texture(source, vUv - kTaps[i] * pc.params.xy);
        outColor = sum / 7.0;
    }
#ifdef MULTISAMPLED
    else if (pc.mode == 7u)
    {
        // The scene's samples (bound as `depth`) into one: their mean where
        // every one is past 1, else the mean of each held to a luminance of
        // 1. A plain mean of a sample near 4 and a dark one is still past
        // white, which leaves a lit edge as jagged as with one sample.
        ivec2 at = ivec2(gl_FragCoord.xy);
        int count = textureSamples(depth);
        vec4 mean = vec4(0.0);
        vec4 held = vec4(0.0);
        float dimmest = 4.0;
        for (int i = 0; i < count; ++i)
        {
            vec4 tap = texelFetch(depth, at, i);
            float lum = dot(tap.rgb, kLuminance);
            dimmest = min(dimmest, lum);
            mean += tap;
            held += vec4(tap.rgb / max(lum, 1.0), tap.a);
        }
        outColor = (dimmest > 1.0 ? mean : held) / float(count);
    }
#endif
    else
    {
        vec4 colour = texture(source, vUv);
        if (pc.sharpen > 0.0)
        {
            // AMD's contrast adaptive sharpening over the pixel and the four
            // beside it, held under 1: each takes away from the middle by
            // how much room the five leave before black or white, so a flat
            // patch stays flat and an edge already hard is not rung.
            ivec2 at = ivec2(gl_FragCoord.xy);
            ivec2 last = textureSize(source, 0) - 1;
            vec3 middle = held(colour.rgb);
            vec3 sum = vec3(0.0), low = middle, high = middle;
            const ivec2 kBeside[4] = ivec2[](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
            for (int i = 0; i < 4; ++i)
            {
                vec3 tap = held(texelFetch(source, clamp(at + kBeside[i], ivec2(0), last), 0).rgb);
                sum += tap;
                low = min(low, tap);
                high = max(high, tap);
            }
            vec3 weight = -pc.sharpen * sqrt(clamp(min(low, 1.0 - high) / max(high, 1.0e-4), 0.0, 1.0));
            colour.rgb = freed(clamp((middle + sum * weight) / (1.0 + 4.0 * weight), low, high));
        }
        if (pc.focus.w > 0.0)
        {
            // Toward the low picture by how far the pixel is from the focus.
            float z = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).r;
            float blur = clamp(abs(z * pc.focus.x + pc.focus.y), pc.focus.z, pc.focus.w);
            colour.rgb = mix(colour.rgb, texture(blurred, vUv).rgb, blur);
        }
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
