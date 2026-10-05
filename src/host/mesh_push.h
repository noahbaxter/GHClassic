#pragma once

#include <cstdint>

// What mesh.vert and mesh.frag are given for one draw, shared by the
// renderer and the draw dump's copy of the vertex shader.
namespace gh2
{
    // How a vertex gets its colour, by the VU1 lighting program the
    // material and environ pick (PsMat::Select 0x3d8548).
    enum ColorMode : uint32_t
    {
        kColorVertex,      // 0x614 (prelit, no environ): as is
        kColorAmbient,     // 0x7c5 with the environ: base * ambient
        kColorDirectional, // 0x6ec: base * ambient + lights * material
        kColorMaterial,    // 0x7c5 without the environ or prelit: the material's
        kColorPoint,       // 0x436: base * ambient + a point light * material, within its range
    };
    constexpr uint32_t kFlagPrelit = 1u << 3;    // base is the vertex colour, else the material's
    constexpr uint32_t kFlagAlphaCut = 1u << 4;  // discard alpha below the GS's 1
    constexpr uint32_t kFlagIntensify = 1u << 5; // textured rgb scale 255 over 128
    constexpr uint32_t kSkinBonesShift = 6;      // two bits: the skin program's bones, less one
    constexpr uint32_t kFlagProjected = 1u << 8; // the tex gen block is the projected one's
    constexpr uint32_t kFlagHighlight = 1u << 9; // GS HIGHLIGHT texturing in place of modulate
    constexpr uint32_t kFlagSphere = 1u << 10;   // the tex gen block is the sphere one's
    constexpr uint32_t kFlagSpread = 1u << 11;   // sampled as the mean of four, matColor.xy apart in uv

    struct PushConstants
    {
        float mvp[16];
        float matColor[4];
        float uvRows[4]; // Material::uvXfm
        float uvOffset[2];
        int32_t boneBase;  // the draw's first bone vec4 in the frame data, -1 when rigid
        int32_t lightBase; // the draw's lighting block in the frame data
        uint32_t flags;    // ColorMode in bits 0-2, then the kFlag bits and the skin bones
        int32_t envBase;   // the draw's environ, projected or sphere tex gen block in the frame data, -1 for none
    };
    static_assert(sizeof(PushConstants) <= 128, "past Vulkan's guaranteed push constant size");

    struct Vec4
    {
        float v[4];
    };
}
