#pragma once

#include <cstdint>

// Where Milo keeps its renderer objects' fields, read from GH2 retail. These
// describe the engine rather than one executable (GH2 and 80s share it), so
// they live apart from the per-executable function addresses.
namespace milo
{
    // Rnd, the renderer. PsRnd::SwapBuffers clears with the clear colour
    // (0x19ad44) and reads the size (0x19ad84, 0x19ad7c); Rnd::YRatio
    // (0x1d4c20) looks the aspect index up in {1, 0.75, 0.5625}.
    namespace rnd
    {
        constexpr uint32_t kClearColor = 0x30u; // Hmx::Color, 4 floats
        constexpr uint32_t kWidth = 0x40u;      // the frame's width in pixels
        constexpr uint32_t kHeight = 0x44u;     // and height
        constexpr uint32_t kAspect = 0xd4u;     // index
        // PsRnd's own, read by VSync (0x19a970, 0x19a9a0).
        constexpr uint32_t kNoDepthOfField = 0x508u; // nonzero: VSync only waits on sceGsSyncV; its meaning is not known
        constexpr uint32_t kFocusZ = 0x510u;         // the GS Z depth of field starts at, 0 for none
    }

    // Hmx::Object, the virtual base an object's first word points to.
    namespace object
    {
        constexpr uint32_t kName = 0x14u; // const char*
        constexpr uint32_t kDir = 0x18u;  // ObjectDir*
    }

    // RndTransformable: local transform at +0x20, world at +0x60. A
    // transform is three 16-byte rows then the position.
    namespace transformable
    {
        constexpr uint32_t kWorld = 0x60u;
    }

    // RndCam, 0x320 bytes (PsCam adds nothing).
    namespace camera
    {
        constexpr uint32_t kView = 0xc0u;   // Transform: inverse of the camera's world
        constexpr uint32_t kNear = 0x2c0u;
        constexpr uint32_t kFar = 0x2c4u;
        constexpr uint32_t kYFov = 0x2c8u;  // 0 is orthographic
        constexpr uint32_t kZRange = 0x2ccu; // 2 floats
        constexpr uint32_t kRect = 0x2d4u;  // normalized x, y, w, h
        constexpr uint32_t kTargetTex = 0x2ecu; // RndTex* (ObjPtr at +0x2e4), null for the screen
    }

    // RndMat, 0x120 bytes; PsMat adds its GS state after. Enums read from
    // PsMat::Update (0x19cfe0) and its jump tables at 0x418910 and 0x418930.
    namespace mat
    {
        constexpr uint32_t kIntensify = 0x28u;  // bool: textured colour scale 255, not 128
        constexpr uint32_t kBlend = 0x2cu;      // Blend
        constexpr uint32_t kColor = 0x30u;      // 4 floats
        constexpr uint32_t kUseEnviron = 0x40u; // bool: lit by the current environ
        constexpr uint32_t kZMode = 0x44u;      // ZMode
        constexpr uint32_t kTexGen = 0x48u;     // TexGen
        constexpr uint32_t kTexWrap = 0x4cu;    // 0 clamps
        constexpr uint32_t kTexXfm = 0x50u;     // Transform
        constexpr uint32_t kDiffuseTex = 0x98u; // RndTex* (ObjPtr at +0x90)
        constexpr uint32_t kPrelit = 0x9cu;     // bool: vertex colour is baked light
        constexpr uint32_t kAlphaCut = 0xa0u;   // bool: ATST greater, AREF 0
        constexpr uint32_t kAlphaWrite = 0xa4u; // bool: FBA off, so the frame buffer's alpha is the fragment's (0x3d8498)
        // PsMat's own, set from outside (Track::SetupFade 0x150ac8): DATE,
        // draw only where the frame buffer's alpha bit is clear (0x19d114).
        constexpr uint32_t kDestAlphaTest = 0x120u;
        constexpr uint32_t kNextPass = 0xb0u;   // RndMat* (ObjPtr at +0xa8)
        constexpr uint32_t kPsTexGenRows = 0x170u; // PsMat: three quadwords PsMat::Update derives from tex_xfm

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
    }

    // RndEnviron; PsEnviron adds nothing it reads. From PsEnviron::Select
    // (0x1a2060). Not "environ", which mingw's stdlib.h defines as a macro.
    namespace environment
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
        constexpr uint32_t kTypeFrameBuffer = 0x8u;
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
        constexpr uint32_t kMutable = 0x140u;    // the sync bits that change again: such parts are kept
        constexpr uint32_t kPacket = 0x150u;     // PsMesh's face packet MemHandle*, null until synced
        constexpr uint32_t kPacketQuads = 0x154u; // u16, the packet's length
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
