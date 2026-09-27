#pragma once

#include <cstdint>

// Where Milo keeps its renderer objects' fields, read from GH2 retail. These
// describe the engine rather than one executable (GH2 and 80s share it), so
// they live apart from the per-executable function addresses.
namespace milo
{
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
        constexpr uint32_t kBones = 0x13cu;      // Bones*
        constexpr uint32_t kTransform = 0x40u;   // the RndTransformable base
        constexpr uint32_t kObjectBase = 0x160u; // the Hmx::Object virtual base, in a PsMesh
        constexpr uint32_t kVertSize = 0x40u;
        constexpr uint32_t kFaceSize = 6u;
        // Vert: position +0x00, normal +0x10, colour (4 floats) +0x20,
        // uv +0x30. On skinned meshes the colour holds bone weights.
        constexpr uint32_t kVertPos = 0x00u;
        constexpr uint32_t kVertNormal = 0x10u;
        constexpr uint32_t kVertColor = 0x20u;
        constexpr uint32_t kVertUv = 0x30u;
        // Sync flags: which parts changed.
        constexpr uint32_t kSyncVerts = 0x1fu;
        constexpr uint32_t kSyncFaces = 0x20u;
    }
}
