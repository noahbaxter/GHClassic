// PsMesh::DrawShowing and PsMultiMesh::DrawShowing, native: record the draws
// instead of building packets.
//
// Retail PsMesh (0x3d88d8) draws the owner's geometry with this mesh's own
// world transform, or its bones when it has them, once per material pass.
// Everything else it does is DMA packet work, so nothing in guest state
// depends on it running.

#include "render/native_mesh.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"
#include "render/look.h"
#include "render/mesh_capture.h"
#include "render/native_environ.h"
#include "render/native_mat.h"
#include "settings/settings.h"

#include <algorithm>
#include <cstring>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
    }

    Matrix readTransform(uint8_t *rdram, uint32_t address)
    {
        Matrix m{};
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t col = 0; col < 3; ++col)
                m[row * 4 + col] = load<float>(rdram, address + row * 16u + col * 4u);
        m[15] = 1.0f;
        return m;
    }

    uint32_t currentCamera(uint8_t *rdram)
    {
        const uint32_t cam = load<uint32_t>(rdram, s_addresses->rndCamCurrent);
        Camera camera;
        camera.id = cam;
        if (cam != 0u)
        {
            camera.view = readTransform(rdram, cam + milo::camera::kView);
            for (uint32_t i = 0; i < 3; ++i)
                camera.eye[i] = load<float>(rdram, cam + milo::transformable::kWorld + 0x30u + i * 4u);
            camera.nearPlane = load<float>(rdram, cam + milo::camera::kNear);
            camera.farPlane = load<float>(rdram, cam + milo::camera::kFar);
            camera.yFov = load<float>(rdram, cam + milo::camera::kYFov);
            for (uint32_t i = 0; i < 2; ++i)
                camera.zRange[i] = load<float>(rdram, cam + milo::camera::kZRange + i * 4u);
            for (uint32_t i = 0; i < 4; ++i)
                camera.rect[i] = load<float>(rdram, cam + milo::camera::kRect + i * 4u);
            camera.target = load<uint32_t>(rdram, cam + milo::camera::kTargetTex);
            if (camera.target != 0u)
            {
                camera.targetWidth = load<uint32_t>(rdram, camera.target + milo::tex::kWidth);
                camera.targetHeight = load<uint32_t>(rdram, camera.target + milo::tex::kHeight);
                // The 360 draws its crowd's cards from 256 by 512, twice the
                // PS2's each way, then halves them three times as here.
                if (settings::get(settings::kModels) == settings::kX360)
                {
                    camera.targetWidth *= 2u;
                    camera.targetHeight *= 2u;
                }
            }
        }
        return internCamera(camera);
    }

    namespace
    {

        // One draw per material pass, from this mesh's material, not the
        // owner's.
        void pushPasses(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mesh, DrawCall &draw)
        {
            uint32_t mat = load<uint32_t>(rdram, mesh + milo::mesh::kMat);
            if (mat == 0u)
                mat = load<uint32_t>(rdram, s_addresses->defaultMat);
            for (; mat != 0u; mat = nextPass(rdram, mat))
            {
                draw.material = readMaterial(rdram, mat);
                if (draw.material.texGen == milo::mat::kTexGenSphere)
                    readSphereRows(rdram, ctx, runtime, *s_addresses, mat, draw.material);
                building().draws.push_back(draw);
            }
        }

        // A transformable's world transform, through the engine's own WorldXfm.
        Matrix worldXfm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t transformable)
        {
            return readTransform(rdram, static_cast<uint32_t>(runtime->callGuestFunction(
                                            rdram, ctx, s_addresses->worldXfm, {transformable})));
        }

        // The bone palette DrawShowing uploads for a skinned mesh (0x3d8974):
        // each bone's bind transform, then its object's world. A missing bone
        // after the first repeats the first. The fifth matrix, which the
        // lighting programs take normals through, is identity when a second
        // bone exists, else the first bone's.
        bool readBones(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t bones, DrawCall &draw)
        {
            uint32_t count = 1;
            for (uint32_t b = 0; b < milo::mesh::kBoneCount; ++b)
            {
                const uint32_t object =
                    load<uint32_t>(rdram, bones + milo::mesh::kBoneObject + b * milo::mesh::kBoneObjectStride);
                if (object == 0u)
                {
                    if (b == 0u)
                        return false; // retail would dereference null
                    draw.bones[b] = draw.bones[0];
                    continue;
                }
                count = b + 1u;
                const Matrix bind =
                    readTransform(rdram, bones + milo::mesh::kBoneBind + b * milo::mesh::kBoneBindStride);
                draw.bones[b] = multiply(bind, worldXfm(rdram, ctx, runtime, object));
            }
            draw.skinned = true;
            draw.skinBones = count;
            draw.world = identity();
            draw.lightWorld = count > 1u ? identity() : draw.bones[0];
            return true;
        }

        // The environ the 360 draws one of the track's meshes under, where
        // the PS2's scene has one for the whole track: its board's, and its
        // bar and beat lines', found by name in the track's scene. `out` is
        // left as it is for any other mesh, and where the scene has none.
        // Calls into the guest, with its stack moved down past the name for
        // ObjectDir::FindObject.
        void trackEnviron(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mesh, Environ &out)
        {
            // Only the track's environ fades.
            const uint32_t mat = load<uint32_t>(rdram, mesh + milo::mesh::kMat);
            if (!look::hasBoard() || mat == 0u || !out.fade)
                return;
            std::string name;
            if (look::isBoard(mat))
                name = look::kBoardEnviron;
            else if (const uint32_t matName = load<uint32_t>(rdram, mat + milo::object::kName); matName != 0u)
            {
                const char *text = reinterpret_cast<const char *>(getMemPtr(rdram, matName));
                for (const char *line : look::kLineMats)
                    if (std::strcmp(text, line) == 0)
                        name = look::kLineEnviron;
            }
            if (name.empty())
                return;
            // The board's mesh and the environ it is drawn under are the
            // track code's copies, in no directory; the mesh it was copied
            // from, which owns its geometry, is the scene's.
            const uint32_t owner = load<uint32_t>(rdram, mesh + milo::mesh::kOwner);
            const uint32_t dir =
                owner != 0u ? load<uint32_t>(rdram, owner + milo::mesh::kObjectBase + milo::object::kDir) : 0u;
            if (dir == 0u)
                return;
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t text = (sp - 0x40u - static_cast<uint32_t>(name.size())) & ~0xfu;
            std::memcpy(getMemPtr(rdram, text), name.c_str(), name.size() + 1u);
            SET_GPR_U32(ctx, 29, text - 0x20u);
            const uint32_t env = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, s_addresses->objectDirFindObject, {dir, text, 0u}));
            if (env != 0u)
                out = readEnviron(rdram, ctx, runtime, env);
            SET_GPR_U32(ctx, 29, sp);
        }

        void drawShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t mesh = GPR_U32(ctx, 4);
            const uint32_t owner = load<uint32_t>(rdram, mesh + milo::mesh::kOwner);
            std::shared_ptr<const MeshData> geometry = capturedMesh(rdram, owner);
            // Retail draws nothing, and calls nothing, until the owner has a
            // packet (0x3d890c), which a sync with faces and verts gives it.
            if (geometry && !geometry->indices.empty() && !geometry->verts.empty())
            {
                DrawCall draw;
                const uint32_t bones = load<uint32_t>(rdram, mesh + milo::mesh::kBones);
                if (bones == 0u)
                {
                    draw.world = worldXfm(rdram, ctx, runtime, mesh + milo::mesh::kTransform);
                    draw.lightWorld = draw.world;
                }
                else if (!readBones(rdram, ctx, runtime, bones, draw))
                {
                    ctx->pc = returnTo;
                    return;
                }
                draw.mesh = std::move(geometry);
                draw.environment = currentEnviron();
                trackEnviron(rdram, ctx, runtime, mesh, draw.environment);
                const Frame &frame = building();
                if (frame.casting >= 0)
                {
                    // Into the shadow map, whatever its material.
                    draw.camera = frame.shadows[static_cast<size_t>(frame.casting)];
                    draw.shadowCaster = true;
                    building().draws.push_back(draw);
                }
                else
                {
                    draw.camera = currentCamera(rdram);
                    if (frame.lit >= 0 && static_cast<size_t>(frame.lit) < frame.shadows.size())
                        draw.shadow = static_cast<uint32_t>(frame.lit) + 1u;
                    pushPasses(rdram, ctx, runtime, mesh, draw);
                }
            }
            ctx->pc = returnTo;
        }

        // PsMultiMesh::DrawShowing, retail 0x1a2f00: the mesh drawn at each
        // instance's transform, once per material pass, gated on the owner's
        // packet as PsMesh is (GetMultiMeshPacket, 0x19e208). An instance
        // takes the camera's world rotation, keeping its own position, when
        // the mesh asks to face the camera. The rest is packet work.
        void drawMultiShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t multi = GPR_U32(ctx, 4);
            if (building().casting >= 0) // only a character's meshes cast
            {
                ctx->pc = returnTo;
                return;
            }
            const uint32_t mesh = load<uint32_t>(rdram, multi + milo::multimesh::kMesh);
            const uint32_t owner = mesh != 0u ? load<uint32_t>(rdram, mesh + milo::mesh::kOwner) : 0u;
            std::shared_ptr<const MeshData> geometry = owner != 0u ? capturedMesh(rdram, owner) : nullptr;
            const uint32_t sentinel = multi + milo::multimesh::kInstances;
            const uint32_t first = load<uint32_t>(rdram, sentinel);
            if (geometry && !geometry->indices.empty() && !geometry->verts.empty() && first != sentinel)
            {
                // Retail asks for the camera's world on every draw (0x1a31d0).
                const Matrix cameraWorld =
                    worldXfm(rdram, ctx, runtime, load<uint32_t>(rdram, s_addresses->rndCamCurrent));
                const bool faceCamera = load<uint32_t>(rdram, mesh + milo::mesh::kInstanceMode) == milo::mesh::kFaceCamera;
                DrawCall draw;
                draw.mesh = std::move(geometry);
                draw.environment = currentEnviron();
                trackEnviron(rdram, ctx, runtime, mesh, draw.environment);
                draw.camera = currentCamera(rdram);
                for (uint32_t node = first; node != sentinel; node = load<uint32_t>(rdram, node))
                {
                    draw.world = readTransform(rdram, node + milo::multimesh::kInstanceXfm);
                    if (faceCamera)
                        std::copy(cameraWorld.begin(), cameraWorld.begin() + 12, draw.world.begin());
                    draw.lightWorld = draw.world;
                    pushPasses(rdram, ctx, runtime, mesh, draw);
                }
            }
            ctx->pc = returnTo;
        }
    }

    void installNativeMesh(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.psMeshDrawShowing, drawShowing);
        runtime.replaceFunction(addresses.psMultiMeshDrawShowing, drawMultiShowing);
    }
}
