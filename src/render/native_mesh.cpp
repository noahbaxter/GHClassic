// PsMesh::DrawShowing and PsMultiMesh::DrawShowing, native: record the draws
// instead of building packets.
//
// Retail PsMesh (0x3d88d8) draws the owner's geometry with this mesh's own
// world transform, or its bones when it has them, once per material pass.
// Everything else it does is DMA packet work, so nothing in guest state
// depends on it running.

#include "render/native_mesh.h"

#include "dev/draw_dump.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "render/camera.h"
#include "render/frame.h"
#include "render/mesh_capture.h"
#include "render/native_environ.h"
#include "render/native_mat.h"
#include "settings/settings.h"

#include <algorithm>
#include <cmath>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // The name of the Hmx::Object an object's first word points to.
        std::string objectName(uint8_t *rdram, uint32_t address)
        {
            const uint32_t object = address != 0u ? load<uint32_t>(rdram, address) : 0u;
            const uint32_t name = object != 0u ? load<uint32_t>(rdram, object + milo::object::kName) : 0u;
            return name != 0u ? reinterpret_cast<const char *>(getMemPtr(rdram, name)) : "";
        }
        struct ShadowMapTag;
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
            }
        }
        return internCamera(camera);
    }

    namespace
    {

        // The 360's character shadow maps. The PS2's Character draws one too
        // where the graphics mode is the new one, which it never is on a
        // PS2 (Character::DrawShowing 0x161c68: at its highest LOD, drawn
        // the usual way, with self_shadow set). DrawShadowBuffer (0x1619c0)
        // aims a camera from the environ's first directional light at the
        // character's world sphere and draws the character through it, and
        // DrawShowing gives the result to Rnd::SetShadowMap for the
        // character's own draw and takes it back after. Here
        // DrawShadowBuffer is drawShadowBuffer below, and SetShadowMap,
        // empty on the PS2, says which draws a map is for.
        //
        // The camera is the light's own rotation, back from the sphere's
        // centre along the light's y by d = radius / tan(pi/8), with a 45
        // degree field of view, from d - radius to d + radius. The PS2's
        // code has sin where that has tan; a Xenia frame's three shadow
        // matrices all have a far to near of 2.41421, which is tan's
        // (.planning/gh2x-look/xenia/shadow_matrices.json).
        constexpr float kPi = 3.14159265f;
        int32_t s_made = -1; // the map DrawShadowBuffer last drew, until SetShadowMap takes it up
        int32_t s_lit = -1;  // the map the meshes now drawn are lit under

        // The inverse of a transform: three rows and a position.
        Matrix inverse(const Matrix &m)
        {
            const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
            const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
            const double r[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                                 (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                                 (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
            Matrix out{};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                {
                    out[static_cast<size_t>(row * 4 + col)] = static_cast<float>(r[row * 3 + col]);
                    out[static_cast<size_t>(12 + col)] -= static_cast<float>(m[static_cast<size_t>(12 + row)] * r[row * 3 + col]);
                }
            out[15] = 1.0f;
            return out;
        }

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

        // Whether the mesh is a card a crowd's flat member is drawn on: one
        // under the WorldCrowd's material (BuildBillboard, 0x26bba0).
        bool card(uint8_t *rdram, uint32_t mesh)
        {
            return crowdCard(load<uint32_t>(rdram, mesh + milo::mesh::kMat));
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
                    draw.crowd = card(rdram, mesh);
                }
                else if (!readBones(rdram, ctx, runtime, bones, draw))
                {
                    ctx->pc = returnTo;
                    return;
                }
                else
                {
                    // A crowd member drawn whole is a Character of its own,
                    // named crowd_ (gh1/venues.cpp, crowd).
                    const uint32_t object = load<uint32_t>(rdram, mesh);
                    const uint32_t dir = object != 0u ? load<uint32_t>(rdram, object + milo::object::kDir) : 0u;
                    draw.crowd = dir != 0u && objectName(rdram, dir).rfind("crowd", 0) == 0;
                }
                draw.mesh = std::move(geometry);
                draw.environment =currentEnviron();
                if (drawDumpPending())
                    draw.name = objectName(rdram, mesh);
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
                    if (s_lit >= 0 && static_cast<size_t>(s_lit) < frame.shadows.size())
                        draw.shadow = static_cast<uint32_t>(s_lit) + 1u;
                    pushPasses(rdram, ctx, runtime, mesh, draw);
                }
            }
            ctx->pc = returnTo;
        }

        // Character::DrawShadowBuffer, native.
        void drawShadowBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t self = GPR_U32(ctx, 4);
            ctx->pc = returnTo;
            s_made = -1;
            Frame &frame = building();
            if (settings::get(settings::kShading) != settings::kX360 || frame.casting >= 0 ||
                frame.shadows.size() >= kMaxShadows)
                return;
            uint32_t light = 0u;
            const uint32_t env = load<uint32_t>(rdram, s_addresses->rndEnvironCurrent);
            for (uint32_t node = env != 0u ? load<uint32_t>(rdram, env + milo::environment::kFirstLight) : 0u;
                 node != 0u && light == 0u; node = load<uint32_t>(rdram, node + 4u))
                if (load<uint32_t>(rdram, load<uint32_t>(rdram, node) + milo::light::kType) ==
                    milo::light::kDirectional)
                    light = load<uint32_t>(rdram, node);
            if (light == 0u)
                return;

            // The guest's stack is moved down past a Sphere for the calls.
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t sphere = (sp - 0x40u) & ~0xfu;
            SET_GPR_U32(ctx, 29, sphere - 0x20u);
            const uint32_t vtable = load<uint32_t>(rdram, self + milo::character::kVtable);
            const auto call = [&](uint32_t slot, uint32_t a1, uint32_t a2)
            {
                const uint32_t drawable = self + milo::character::kDrawable +
                                          static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + slot)));
                runtime->callGuestFunction(rdram, ctx, load<uint32_t>(rdram, vtable + slot + 4u), {drawable, a1, a2});
            };
            store<float>(rdram, sphere + 0x10u, 0.0f);
            call(milo::character::kMakeWorldSphere, sphere, 0u);
            const float radius = load<float>(rdram, sphere + 0x10u);
            if (radius > 0.0f)
            {
                const float back = radius / std::tan(kPi / 8.0f);
                Matrix world = worldXfm(rdram, ctx, runtime, light);
                for (uint32_t i = 0; i < 3; ++i)
                    world[12u + i] = load<float>(rdram, sphere + i * 4u) - back * world[4u + i];
                const int32_t index = static_cast<int32_t>(frame.shadows.size());
                Camera camera;
                camera.id = kShadowTex + static_cast<uint32_t>(index);
                camera.view = inverse(world);
                std::copy_n(world.data() + 12, 3, camera.eye);
                camera.nearPlane = back - radius;
                camera.farPlane = back + radius;
                camera.yFov = kPi / 4.0f;
                camera.target = camera.id;
                camera.targetWidth = camera.targetHeight = kShadowSize;
                frame.shadows.push_back(internCamera(camera));
                frame.casting = index;
                call(milo::character::kDrawShowing, milo::character::kDrawShadow, 0u);
                frame.casting = -1;
                s_made = index;            }
            SET_GPR_U32(ctx, 29, sp);
            ctx->pc = returnTo;
        }

        // Rnd::SetShadowMap(tex, cam): Character::DrawShowing's, with the
        // shadow buffer just drawn before the character's own draw and with
        // nothing after it.
        void onSetShadowMap(uint8_t *, R5900Context *, PS2Runtime *)
        {
            s_lit = s_made;
            s_made = -1;
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
                draw.environment =currentEnviron();
                draw.camera = currentCamera(rdram);
                draw.crowd = card(rdram, mesh);
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
        runtime.replaceFunction(addresses.characterDrawShadowBuffer, drawShadowBuffer);
        EntryHook<ShadowMapTag>::install(runtime, addresses.rndSetShadowMap, onSetShadowMap);
    }
}
