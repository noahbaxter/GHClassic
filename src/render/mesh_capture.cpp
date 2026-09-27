// Mesh geometry in the engine's neutral form, taken when a mesh syncs.
//
// PsMesh::Sync converts verts and faces into a PS2 face packet and, for
// static meshes, frees them. The copy is taken on entry, before that. The
// conversion itself still runs, because its strip bookkeeping (a global list
// that loading fills and the conversion drains) has to stay balanced until
// Sync is replaced whole.

#include "render/mesh_capture.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"

#include <unordered_map>

namespace gh2
{
    namespace
    {
        std::unordered_map<uint32_t, std::shared_ptr<const MeshData>> s_meshes;

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
                    dst.pos[c] = load<float>(rdram, v + milo::mesh::kVertPos + c * 4u);
                    dst.normal[c] = load<float>(rdram, v + milo::mesh::kVertNormal + c * 4u);
                }
                for (uint32_t c = 0; c < 4; ++c)
                    dst.color[c] = load<float>(rdram, v + milo::mesh::kVertColor + c * 4u);
                for (uint32_t c = 0; c < 2; ++c)
                    dst.uv[c] = load<float>(rdram, v + milo::mesh::kVertUv + c * 4u);
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
        struct CopyTag;
        struct DestroyTag;

        // PsMesh::Sync(flags). Retail acts only when the mesh owns its data.
        void onSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t mesh = GPR_U32(ctx, 4);
            const uint32_t flags = GPR_U32(ctx, 5);
            if (load<uint32_t>(rdram, mesh + milo::mesh::kOwner) != mesh)
                return;
            std::shared_ptr<const MeshData> &slot = s_meshes[mesh];
            auto next = slot ? std::make_shared<MeshData>(*slot) : std::make_shared<MeshData>();
            if (flags & milo::mesh::kSyncVerts)
                readVerts(rdram, mesh, *next);
            if (flags & milo::mesh::kSyncFaces)
                readFaces(rdram, mesh, *next);
            slot = std::move(next);
        }

        // PsMesh::Copy(from, type). A clone of a static mesh gets its
        // geometry only through the copied packet, so it takes the source's
        // captured geometry here. `from` is the source's Hmx::Object, which
        // retail turns back into a PsMesh with __dynamic_cast (0x19dc8c).
        void onCopy(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t mesh = GPR_U32(ctx, 4);
            const uint32_t from = GPR_U32(ctx, 5);
            if (from < milo::mesh::kObjectBase)
                return;
            const auto source = s_meshes.find(from - milo::mesh::kObjectBase);
            if (source != s_meshes.end())
                s_meshes[mesh] = source->second;
        }

        void onDestroy(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_meshes.erase(GPR_U32(ctx, 4));
        }
    }

    std::shared_ptr<const MeshData> capturedMesh(uint32_t mesh)
    {
        const auto found = s_meshes.find(mesh);
        return found != s_meshes.end() ? found->second : nullptr;
    }

    void installMeshCapture(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<SyncTag>::install(runtime, addresses.psMeshSync, onSync);
        EntryHook<CopyTag>::install(runtime, addresses.psMeshCopy, onCopy);
        EntryHook<DestroyTag>::install(runtime, addresses.psMeshDestroy, onDestroy);
    }
}
