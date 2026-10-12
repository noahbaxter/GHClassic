// Mesh geometry in the engine's neutral form, taken when a mesh syncs.
//
// PsMesh::Sync (0x3d4f08) has UpdateFacePacket turn verts and faces into a
// VU1 packet and, for static meshes, frees them. The copy is taken on entry,
// before that, and no packet is built: nothing native reads one, and the
// 196608 bytes retail builds one in (0x3d48d4) hold no larger mesh.

#include "render/mesh_capture.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"
#include "dev/mesh_packet.h"

#include <unordered_map>

namespace gh2
{
    namespace
    {
        std::unordered_map<uint32_t, std::shared_ptr<const MeshData>> s_meshes;
        bool s_fromPackets = false;
        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_updateFacePacket = nullptr;
        PS2Runtime::RecompiledFunction s_copy = nullptr;

        void readVerts(uint8_t *rdram, uint32_t mesh, MeshData &out)
        {
            const uint32_t verts = load<uint32_t>(rdram, mesh + milo::mesh::kVerts);
            const int32_t count = load<int32_t>(rdram, mesh + milo::mesh::kVertCount);
            out.verts.resize(count > 0 && verts != 0u ? static_cast<size_t>(count) : 0u);
            for (size_t i = 0; i < out.verts.size(); ++i)
            {
                const uint32_t v = verts + static_cast<uint32_t>(i) * milo::mesh::kVertSize;
                Vertex &dst = out.verts[i];
                for (uint32_t c = 0; c < 3; ++c)
                {
                    dst.pos[c] = vuFloat(load<float>(rdram, v + milo::mesh::kVertPos + c * 4u));
                    dst.normal[c] = vuFloat(load<float>(rdram, v + milo::mesh::kVertNormal + c * 4u));
                }
                for (uint32_t c = 0; c < 4; ++c)
                    dst.color[c] = vuFloat(load<float>(rdram, v + milo::mesh::kVertColor + c * 4u));
                for (uint32_t c = 0; c < 2; ++c)
                    dst.uv[c] = vuFloat(load<float>(rdram, v + milo::mesh::kVertUv + c * 4u));
            }
        }

        void readFaces(uint8_t *rdram, uint32_t mesh, MeshData &out)
        {
            const uint32_t begin = load<uint32_t>(rdram, mesh + milo::mesh::kFacesBegin);
            const uint32_t end = load<uint32_t>(rdram, mesh + milo::mesh::kFacesEnd);
            const uint32_t count = end > begin ? (end - begin) / milo::mesh::kFaceSize : 0u;
            out.indices.resize(static_cast<size_t>(count) * 3u);
            for (uint32_t i = 0; i < count * 3u; ++i)
                out.indices[i] = load<uint16_t>(rdram, begin + i * 2u);
        }

        struct SyncTag;
        struct DestroyTag;

        // PsMesh::Sync(flags). Retail acts only when the mesh owns its data.
        void onSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t mesh = GPR_U32(ctx, 4);
            const uint32_t flags = GPR_U32(ctx, 5);
            if (load<uint32_t>(rdram, mesh + milo::mesh::kOwner) != mesh || s_fromPackets)
                return;
            std::shared_ptr<const MeshData> &slot = s_meshes[mesh];
            auto next = slot ? std::make_shared<MeshData>(*slot) : std::make_shared<MeshData>();
            if (flags & milo::mesh::kSyncVerts)
                readVerts(rdram, mesh, *next);
            if (flags & milo::mesh::kSyncFaces)
                readFaces(rdram, mesh, *next);
            slot = std::move(next);
        }

        // PsMesh::UpdateFacePacket, retail 0x3d47f0, less the packet: the old
        // one let go (0x3d4828), and for a mesh with faces and verts (0x3d4838,
        // 0x3d4844) one entry of the strips its file came with taken off their
        // list for each face group (0x3d4ae4), as loading put them there.
        void updateFacePacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_fromPackets)
            {
                s_updateFacePacket(rdram, ctx, runtime);
                return;
            }
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t mesh = GPR_U32(ctx, 4);
            runtime->callGuestFunction(rdram, ctx, s_addresses->psRndSyncFree,
                                       {s_addresses->thePsRnd, load<uint32_t>(rdram, mesh + milo::mesh::kPacket)});
            store<uint32_t>(rdram, mesh + milo::mesh::kPacket, 0u);
            const bool faces = load<uint32_t>(rdram, mesh + milo::mesh::kFacesBegin) !=
                               load<uint32_t>(rdram, mesh + milo::mesh::kFacesEnd);
            if (faces && load<uint32_t>(rdram, mesh + milo::mesh::kVertCount) != 0u)
            {
                const uint32_t groups = mesh + milo::mesh::kGroups;
                const uint32_t sentinel = s_addresses->meshStrips + 4u;
                for (uint32_t group = load<uint32_t>(rdram, groups); group != groups; group = load<uint32_t>(rdram, group))
                {
                    const uint32_t node = load<uint32_t>(rdram, sentinel);
                    if (node == sentinel)
                        break;
                    runtime->callGuestFunction(rdram, ctx, s_addresses->memFree, {load<uint32_t>(rdram, node + 0xcu)});
                    const uint32_t next = load<uint32_t>(rdram, node);
                    const uint32_t prev = load<uint32_t>(rdram, node + 4u);
                    store<uint32_t>(rdram, prev, next);
                    store<uint32_t>(rdram, next + 4u, prev);
                    runtime->callGuestFunction(rdram, ctx, s_addresses->memOrPoolFree, {0x18u, node});
                }
            }
            ctx->pc = returnTo;
        }

        // PsMesh::Copy, retail 0x19dc38. RndMesh::Copy syncs this mesh from
        // what it holds. A clone of a static mesh holds no verts, so that
        // sync captured nothing and the source's geometry is what it draws;
        // a clone that kept verts keeps its own.
        void copy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t mesh = GPR_U32(ctx, 4);
            const uint32_t from = GPR_U32(ctx, 5);
            runtime->callGuestFunction(rdram, ctx, s_addresses->psMeshCopy, {mesh, from, GPR_U32(ctx, 6)}, s_copy);
            // The object `from` is part of, as the cast finds it (0x19dc78).
            const uint32_t whole =
                from != 0u ? from + static_cast<int32_t>(load<int16_t>(rdram, load<uint32_t>(rdram, from))) : 0u;
            const auto source = s_meshes.find(whole);
            if (source != s_meshes.end() && load<uint32_t>(rdram, mesh + milo::mesh::kOwner) == mesh)
            {
                std::shared_ptr<const MeshData> &slot = s_meshes[mesh];
                if (!slot || slot->verts.empty())
                    slot = source->second;
            }
            ctx->pc = returnTo;
        }

        void onDestroy(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_meshes.erase(GPR_U32(ctx, 4));
        }
    }

    std::shared_ptr<const MeshData> capturedMesh(uint8_t *rdram, uint32_t mesh)
    {
        if (s_fromPackets)
            return decodeMeshPacket(rdram, mesh);
        const auto found = s_meshes.find(mesh);
        return found != s_meshes.end() ? found->second : nullptr;
    }

    void readMeshesFromPackets()
    {
        s_meshes.clear();
        s_fromPackets = true;
    }

    void installMeshCapture(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<SyncTag>::install(runtime, addresses.psMeshSync, onSync);
        s_updateFacePacket = runtime.lookupFunction(addresses.psMeshUpdateFacePacket);
        runtime.replaceFunction(addresses.psMeshUpdateFacePacket, updateFacePacket);
        s_copy = runtime.lookupFunction(addresses.psMeshCopy);
        runtime.replaceFunction(addresses.psMeshCopy, copy);
        EntryHook<DestroyTag>::install(runtime, addresses.psMeshDestroy, onDestroy);
    }
}
