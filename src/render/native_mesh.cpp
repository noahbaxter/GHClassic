// PsMesh::DrawShowing, native: record the draw instead of building packets.
//
// Retail (0x3d88d8) draws the owner's geometry with this mesh's own world
// transform, or its bones when it has them, once per material pass.
// Everything else it does is DMA packet work, so nothing in guest state
// depends on it running.

#include "render/native_mesh.h"

#include "guest.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"
#include "render/mesh_capture.h"
#include "render/native_environ.h"
#include "render/native_mat.h"

#include <cstring>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // A Transform in guest memory: three 16-byte rows, then the position.
        Matrix readTransform(uint8_t *rdram, uint32_t address)
        {
            Matrix m{};
            for (uint32_t row = 0; row < 4; ++row)
                for (uint32_t col = 0; col < 3; ++col)
                    m[row * 4 + col] = load<float>(rdram, address + row * 16u + col * 4u);
            m[15] = 1.0f;
            return m;
        }

        // The camera current when the draw is made, reusing the last entry
        // when nothing about it has changed.
        uint32_t currentCamera(uint8_t *rdram)
        {
            Frame &frame = building();
            const uint32_t cam = load<uint32_t>(rdram, s_addresses->rndCamCurrent);
            Camera camera;
            camera.id = cam;
            if (cam != 0u)
            {
                camera.view = readTransform(rdram, cam + milo::camera::kView);
                camera.nearPlane = load<float>(rdram, cam + milo::camera::kNear);
                camera.farPlane = load<float>(rdram, cam + milo::camera::kFar);
                camera.yFov = load<float>(rdram, cam + milo::camera::kYFov);
                for (uint32_t i = 0; i < 2; ++i)
                    camera.zRange[i] = load<float>(rdram, cam + milo::camera::kZRange + i * 4u);
                for (uint32_t i = 0; i < 4; ++i)
                    camera.rect[i] = load<float>(rdram, cam + milo::camera::kRect + i * 4u);
            }
            if (!frame.cameras.empty() && std::memcmp(&frame.cameras.back(), &camera, sizeof(Camera)) == 0)
                return static_cast<uint32_t>(frame.cameras.size() - 1u);
            frame.cameras.push_back(camera);
            return static_cast<uint32_t>(frame.cameras.size() - 1u);
        }

        // The bone palette DrawShowing uploads for a skinned mesh (0x3d8974):
        // each bone's bind transform, then its object's world. A missing bone
        // after the first repeats the first. The fifth matrix, which the
        // lighting programs take normals through, is identity when a second
        // bone exists, else the first bone's.
        template <typename WorldXfm>
        bool readBones(uint8_t *rdram, uint32_t bones, const WorldXfm &worldXfm, DrawCall &draw)
        {
            bool several = false;
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
                several |= b != 0u;
                const Matrix bind =
                    readTransform(rdram, bones + milo::mesh::kBoneBind + b * milo::mesh::kBoneBindStride);
                draw.bones[b] = multiply(bind, worldXfm(object));
            }
            draw.skinned = true;
            draw.world = identity();
            draw.lightWorld = several ? identity() : draw.bones[0];
            return true;
        }

        void drawShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t mesh = GPR_U32(ctx, 4);
            const uint32_t owner = load<uint32_t>(rdram, mesh + milo::mesh::kOwner);
            std::shared_ptr<const MeshData> geometry = capturedMesh(owner);
            // Retail draws nothing, and calls nothing, until the owner has a
            // packet (0x3d890c).
            const bool synced = load<uint32_t>(rdram, owner + milo::mesh::kPacket) != 0u;
            if (synced && geometry && !geometry->indices.empty())
            {
                const auto worldXfm = [&](uint32_t transformable) {
                    return readTransform(rdram, static_cast<uint32_t>(runtime->callGuestFunction(
                                                    rdram, ctx, s_addresses->worldXfm, {transformable})));
                };
                DrawCall draw;
                const uint32_t bones = load<uint32_t>(rdram, mesh + milo::mesh::kBones);
                if (bones == 0u)
                {
                    draw.world = worldXfm(mesh + milo::mesh::kTransform);
                    draw.lightWorld = draw.world;
                }
                else if (!readBones(rdram, bones, worldXfm, draw))
                {
                    ctx->pc = returnTo;
                    return;
                }
                draw.mesh = std::move(geometry);
                draw.environ = currentEnviron();
                draw.camera = currentCamera(rdram);
                // One draw per material pass, from this mesh's material, not
                // the owner's.
                uint32_t mat = load<uint32_t>(rdram, mesh + milo::mesh::kMat);
                if (mat == 0u)
                    mat = load<uint32_t>(rdram, s_addresses->defaultMat);
                for (; mat != 0u; mat = nextPass(rdram, mat))
                {
                    draw.material = readMaterial(rdram, mat);
                    building().draws.push_back(draw);
                }
            }
            ctx->pc = returnTo;
        }
    }

    void installNativeMesh(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        runtime.replaceFunction(addresses.psMeshDrawShowing, drawShowing);
    }
}
