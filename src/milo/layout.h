#pragma once

#include <cstddef>
#include <cstdint>

// Where Milo keeps its renderer objects' fields, read from GH2 retail. These
// describe the engine rather than one executable (GH2 and 80s share it), so
// they live apart from the per-executable function addresses.
//
// A struct here is a guest object as retail lays it out, read whole with
// load<T>. Only the fields something reads are named; the spans between are
// unkXX, by offset. The names are this project's, from what the code that
// touches a field does with it: no symbols for them survive. A base class is
// the span it occupies, not a C++ base.
//
// A loaded struct is a copy: load it after any guest call that may change
// the object, not before.
namespace milo
{
    // A guest pointer to a T, followed with load(rdram, ptr).
    template <typename T>
    struct Ptr
    {
        uint32_t address;

        explicit operator bool() const
        {
            return address != 0u;
        }
    };

    // ObjPtr<T>: the object it points to is 8 in.
    template <typename T>
    struct ObjPtr
    {
        uint32_t unk0[2];
        Ptr<T> ptr;
    };
    static_assert(sizeof(ObjPtr<void>) == 0xc);

    // Three 16-byte rows, then the position, w unused throughout.
    struct Transform
    {
        float rows[3][4];
        float pos[4];
    };
    static_assert(sizeof(Transform) == 0x40);

    // Hmx::Color.
    struct Color
    {
        float rgba[4];
    };
    static_assert(sizeof(Color) == 0x10);

    // Hmx::Rect.
    struct Rect
    {
        float x, y, w, h;
    };
    static_assert(sizeof(Rect) == 0x10);

    struct RndTex;

    // Rnd, the renderer. PsRnd::SwapBuffers clears with the clear colour
    // (0x19ad44) and reads the size (0x19ad84, 0x19ad7c); Rnd::YRatio
    // (0x1d4c20) looks the aspect index up in {1, 0.75, 0.5625}.
    struct Rnd
    {
        uint8_t unk0[0x30];
        Color clearColor;
        int32_t width;   // the frame's, in pixels
        int32_t height;
        uint8_t unk48[0x8c];
        uint32_t aspect; // index
    };
    static_assert(offsetof(Rnd, clearColor) == 0x30);
    static_assert(offsetof(Rnd, width) == 0x40);
    static_assert(offsetof(Rnd, height) == 0x44);
    static_assert(offsetof(Rnd, aspect) == 0xd4);

    // RndTransformable, as far as anything reads it.
    struct RndTransformable
    {
        uint8_t unk0[0x60]; // the local Transform at +0x20
        Transform world;
    };
    static_assert(sizeof(RndTransformable) == 0xa0);
    static_assert(offsetof(RndTransformable, world) == 0x60);

    // RndCam, 0x320 bytes (PsCam adds nothing).
    struct RndCam
    {
        RndTransformable transformable;
        uint8_t unkA0[0x20];
        Transform view;  // the inverse of its world
        uint8_t unk100[0x1c0];
        float nearPlane;
        float farPlane;
        float yFov;      // 0 is orthographic
        float zRange[2];
        Rect rect;       // normalized
        ObjPtr<RndTex> targetTex; // null for the screen
        uint8_t unk2F0[0x30];
    };
    static_assert(sizeof(RndCam) == 0x320);
    static_assert(offsetof(RndCam, view) == 0xc0);
    static_assert(offsetof(RndCam, nearPlane) == 0x2c0);
    static_assert(offsetof(RndCam, farPlane) == 0x2c4);
    static_assert(offsetof(RndCam, yFov) == 0x2c8);
    static_assert(offsetof(RndCam, zRange) == 0x2cc);
    static_assert(offsetof(RndCam, rect) == 0x2d4);
    static_assert(offsetof(RndCam, targetTex) == 0x2e4);

    // RndMat, 0x120 bytes; PsMat adds its GS state after. Enums read from
    // PsMat::Update (0x19cfe0) and its jump tables at 0x418910 and 0x418930.
    struct RndMat
    {
        enum Blend : uint32_t
        {
            kBlendDest,          // Cd
            kBlendSrc,           // Cs, alpha blending off
            kBlendAdd,           // Cs + Cd
            kBlendSrcAlpha,      // (Cs - Cd) * As + Cd
            kBlendSrcAlphaAdd,   // Cs * As + Cd
            kBlendSubtract,      // Cd - Cs
            kBlendCount,
        };

        // Update's jump table at 0x418950.
        enum TexGen : uint32_t
        {
            kTexGenNone,
            kTexGenXfm,       // uv through tex_xfm, about the texture's centre
            kTexGenSphere,
            kTexGenProjected,
            kTexGenXfmOrigin, // uv through tex_xfm, about the origin
            kTexGenEnviron,
        };

        enum ZMode : uint32_t
        {
            kZDisable,       // always, no write
            kZNormal,        // greater, write
            kZTransparent,   // gequal, no write
            kZForce,         // always, write
            kZDecal,         // gequal, write
            kZModeCount,
        };

        uint8_t unk0[0x28];
        uint32_t intensify;  // bool: textured colour scale 255, not 128
        Blend blend;
        Color color;
        uint32_t useEnviron; // bool: lit by the current environ
        ZMode zMode;
        TexGen texGen;
        uint32_t texWrap;    // 0 clamps
        Transform texXfm;
        ObjPtr<RndTex> diffuseTex;
        uint32_t prelit;     // bool: vertex colour is baked light
        uint32_t alphaCut;   // bool: ATST greater, AREF 0
        uint8_t unkA4[0x4];
        ObjPtr<RndMat> nextPass;
        uint8_t unkB4[0x6c];
    };
    static_assert(sizeof(RndMat) == 0x120);
    static_assert(offsetof(RndMat, intensify) == 0x28);
    static_assert(offsetof(RndMat, blend) == 0x2c);
    static_assert(offsetof(RndMat, color) == 0x30);
    static_assert(offsetof(RndMat, useEnviron) == 0x40);
    static_assert(offsetof(RndMat, zMode) == 0x44);
    static_assert(offsetof(RndMat, texGen) == 0x48);
    static_assert(offsetof(RndMat, texWrap) == 0x4c);
    static_assert(offsetof(RndMat, texXfm) == 0x50);
    static_assert(offsetof(RndMat, diffuseTex) == 0x90);
    static_assert(offsetof(RndMat, prelit) == 0x9c);
    static_assert(offsetof(RndMat, alphaCut) == 0xa0);
    static_assert(offsetof(RndMat, nextPass) == 0xa8);

    // RndEnviron; PsEnviron adds nothing it reads. From PsEnviron::Select
    // (0x1a2060).
    namespace environ
    {
        constexpr uint32_t kFirstLight = 0x30u; // light list node*: {RndLight*, next*}
        constexpr uint32_t kAmbient = 0x40u;    // 3 floats
    }

    // RndLight, a RndTransformable at +0.
    namespace light
    {
        constexpr uint32_t kColor = 0xc0u;  // 4 floats; VU1 gets w as 0
        constexpr uint32_t kRange = 0xd0u;  // point lights
        constexpr uint32_t kType = 0xd4u;   // Type

        enum Type : uint32_t
        {
            kPoint,
            kDirectional, // shines along its world +y
        };
    }

    // RndBitmap, 0x1c bytes.
    namespace bitmap
    {
        constexpr uint32_t kWidth = 0x00u;    // u16
        constexpr uint32_t kHeight = 0x02u;   // u16
        constexpr uint32_t kRowBytes = 0x04u; // u16
        constexpr uint32_t kBpp = 0x06u;      // u8: 4, 8, 16, 24 or 32
        constexpr uint32_t kOrder = 0x08u;    // u32, Order bits
        constexpr uint32_t kPixels = 0x0cu;   // u8*
        constexpr uint32_t kPalette = 0x10u;  // u8*, 4 bytes an entry
        constexpr uint32_t kMip = 0x18u;      // RndBitmap*, the next smaller level (NumMips 0x1ae3a8)

        // Read from ConvertColor (0x1ae538), PaletteOffset (0x1b0f08) and
        // PixelOffset (0x1aed38).
        enum Order : uint32_t
        {
            kRgba = 0x1u,     // bytes R, G, B, A; else B, G, R, A. 16bpp: R in the low bits
            kPs2Alpha = 0x2u, // alpha 0..0x80; for 8bpp also the GS's CLUT entry order
            kSwizzled = 0x4u, // 4 and 8bpp pixels in the order the GS upload wants
        };
    }

    // RndTex, 0x70 bytes, then PsTex's own.
    namespace tex
    {
        constexpr uint32_t kBitmap = 0x28u; // RndBitmap
        constexpr uint32_t kType = 0x48u;   // Type bits
        constexpr uint32_t kWidth = 0x4cu;  // int
        constexpr uint32_t kHeight = 0x50u; // int
        constexpr uint32_t kTypeRendered = 0x2u; // drawn into through a camera's target
        // Only regular textures have pixels in RAM (SyncBitmap 0x1a13c0).
        constexpr uint32_t kTypeNoPixels = 0x2u | 0x4u | 0x8u; // rendered, movie, frame buffer
    }

    // RndText: its font's ObjPtr at +0x114, the pointer 8 in.
    namespace text
    {
        constexpr uint32_t kFont = 0x11cu; // RndFont*
    }

    // RndFont: glyphs are its material's texture (ValidTexture 0x22e4f8).
    namespace font
    {
        constexpr uint32_t kMat = 0x30u; // RndMat*
    }

    // RndMesh, 0x180 bytes; PsMesh adds its packet at +0x150.
    namespace mesh
    {
        constexpr uint32_t kVerts = 0x100u;      // Vert*
        constexpr uint32_t kVertCount = 0x104u;  // int
        constexpr uint32_t kFacesBegin = 0x108u; // Face* (3 x u16)
        constexpr uint32_t kFacesEnd = 0x10cu;
        constexpr uint32_t kMat = 0x120u;        // RndMat* (ObjPtr at +0x118)
        constexpr uint32_t kOwner = 0x138u;      // RndMesh* (ObjPtr at +0x130)
        constexpr uint32_t kBones = 0x13cu;      // Bones*, null for a rigid mesh
        constexpr uint32_t kTransform = 0x40u;   // the RndTransformable base
        constexpr uint32_t kObjectBase = 0x160u; // the Hmx::Object virtual base, in a PsMesh
        constexpr uint32_t kPacket = 0x150u;     // PsMesh's face packet MemHandle*, null until synced
        constexpr uint32_t kVertSize = 0x40u;
        constexpr uint32_t kFaceSize = 6u;
        // Vert: position +0x00, normal +0x10, colour (4 floats) +0x20,
        // uv +0x30. On skinned meshes the colour holds bone weights.
        constexpr uint32_t kVertPos = 0x00u;
        constexpr uint32_t kVertNormal = 0x10u;
        constexpr uint32_t kVertColor = 0x20u;
        constexpr uint32_t kVertUv = 0x30u;
        // Bones: four ObjPtr<RndTransformable> (object at +0x8 of each,
        // 0xc apart), then each bone's bind Transform (0x40 apart).
        constexpr uint32_t kBoneCount = 4u;
        constexpr uint32_t kBoneObject = 0x08u;
        constexpr uint32_t kBoneObjectStride = 0x0cu;
        constexpr uint32_t kBoneBind = 0x30u;
        constexpr uint32_t kBoneBindStride = 0x40u;
        // Sync flags: which parts changed.
        constexpr uint32_t kSyncVerts = 0x1fu;
        constexpr uint32_t kSyncFaces = 0x20u;
        // PsMultiMesh::DrawShowing turns each instance to face the camera
        // when this is kFaceCamera (0x1a31c8). The field's name is unknown.
        constexpr uint32_t kInstanceMode = 0xe4u;
        constexpr uint32_t kFaceCamera = 8u;
    }

    // RndMultiMesh: one mesh drawn at each of a list of transforms.
    namespace multimesh
    {
        constexpr uint32_t kMesh = 0x48u;      // RndMesh* (ObjPtr at +0x40)
        constexpr uint32_t kInstances = 0x50u; // the instance list's sentinel node; its first word is the first node
        constexpr uint32_t kInstanceXfm = 0x10u; // a node's Transform
    }
}
