// PsMesh's packet, read back into verts and triangles.
//
// UpdateFacePacket (0x3d47f0) cuts a mesh into batches VU1 can hold. Each
// batch is VIF1 data ending in an MSCAL or MSCNT:
//
//   UNPACK V4-16 to the strip list   four vert addresses a quadword
//   UNPACK V4-32 to 0                vert count, list start, list end, skin program
//   STCYCL 4, 2; UNPACK V3-32 to 1   each vert's position and normal
//   STCYCL 4, 1; UNPACK V4-32 to 3   each vert's colour, or bone weights
//                UNPACK V2-32 to 4   each vert's uv
//
// A vert sits at 1 + 4 * its index. The list is one strip: the T&L program
// (0x19b, 0x616) takes each address in turn, and one that is not positive
// is its negative with the GS's ADC bit, so it draws no triangle (0x3b6).
//
// A mesh that keeps its verts (PsMesh +0x140, any of the vert sync bits)
// has a DMA chain instead, sent without the tags' own VIF words: cnt tags
// carry the lists and headers, and one ref tag a vert points into the vert
// array (DrawFaces 0x3d87ec).

#include "dev/mesh_packet.h"

#include "guest.h"
#include "milo/layout.h"

#include <array>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kRamEnd = 0x2000000u;
        constexpr uint32_t kVuQuads = 1024u;

        // The words VIF1 is sent for the mesh: the packet, or what its chain
        // of cnt and ref tags gathers.
        bool readStream(uint8_t *rdram, uint32_t mesh, std::vector<uint32_t> &out)
        {
            const uint32_t handle = load<uint32_t>(rdram, mesh + milo::mesh::kPacket);
            if (handle == 0u)
                return false;
            const uint32_t packet = load<uint32_t>(rdram, handle) + 0x10u; // MemHandle::Lock, 0x2cede8
            const uint32_t quads = load<uint16_t>(rdram, mesh + milo::mesh::kPacketQuads);
            if (packet >= kRamEnd || quads * 16u > kRamEnd - packet)
                return false;
            const auto append = [&](uint32_t from, uint32_t count) {
                if (from >= kRamEnd || count * 16u > kRamEnd - from)
                    return false;
                const size_t at = out.size();
                out.resize(at + count * 4u);
                std::memcpy(out.data() + at, getMemPtr(rdram, from), count * 16u);
                return true;
            };
            if ((load<uint32_t>(rdram, mesh + milo::mesh::kMutable) & milo::mesh::kSyncVerts) == 0u)
                return append(packet, quads);
            for (uint32_t q = 0; q < quads;)
            {
                const uint32_t tag = load<uint32_t>(rdram, packet + q * 16u);
                const uint32_t count = tag & 0xffffu;
                const uint32_t id = (tag >> 28) & 7u;
                if (id == 1u) // cnt: the data follows
                {
                    if (q + 1u + count > quads || !append(packet + (q + 1u) * 16u, count))
                        return false;
                    q += 1u + count;
                }
                else if (id == 3u) // ref: the data is elsewhere
                {
                    if (!append(load<uint32_t>(rdram, packet + q * 16u + 4u), count))
                        return false;
                    q += 1u;
                }
                else
                    return false;
            }
            return true;
        }

        // One batch's VU1 memory to verts and triangles.
        bool takeBatch(const std::vector<std::array<uint32_t, 4>> &vu, MeshData &out)
        {
            const uint32_t count = vu[0][0];
            const uint32_t listBegin = vu[0][1];
            const uint32_t listEnd = vu[0][2];
            if (1u + count * 4u > kVuQuads || listBegin > listEnd || listEnd > kVuQuads)
                return false;
            const size_t base = out.verts.size();
            if (base + count > 0x10000u)
                return false;
            for (uint32_t i = 0; i < count; ++i)
            {
                Vertex v{};
                std::memcpy(v.pos, vu[1u + i * 4u].data(), sizeof(v.pos));
                std::memcpy(v.normal, vu[2u + i * 4u].data(), sizeof(v.normal));
                std::memcpy(v.color, vu[3u + i * 4u].data(), sizeof(v.color));
                std::memcpy(v.uv, vu[4u + i * 4u].data(), sizeof(v.uv));
                for (float &f : v.pos)
                    f = vuFloat(f);
                for (float &f : v.normal)
                    f = vuFloat(f);
                for (float &f : v.color)
                    f = vuFloat(f);
                for (float &f : v.uv)
                    f = vuFloat(f);
                out.verts.push_back(v);
            }
            uint16_t last[2] = {};
            uint32_t seen = 0;
            for (uint32_t q = listBegin; q < listEnd; ++q)
                for (uint32_t c = 0; c < 4; ++c, ++seen)
                {
                    const int32_t entry = static_cast<int32_t>(vu[q][c]);
                    const uint32_t address = static_cast<uint32_t>(entry > 0 ? entry : -entry);
                    // A padding entry is 0: the header's quadword, never drawn.
                    const uint32_t index = address != 0u ? (address - 1u) / 4u : 0u;
                    if (index >= count)
                        return false;
                    const uint16_t vert = static_cast<uint16_t>(base + index);
                    if (entry > 0 && seen >= 2u)
                    {
                        const bool odd = (seen & 1u) != 0u;
                        out.indices.push_back(last[odd ? 1 : 0]);
                        out.indices.push_back(last[odd ? 0 : 1]);
                        out.indices.push_back(vert);
                    }
                    last[0] = last[1];
                    last[1] = vert;
                }
            return true;
        }

        struct Decoded
        {
            uint32_t hash = 0;
            std::shared_ptr<const MeshData> data;
        };
        std::unordered_map<uint32_t, Decoded> s_decoded;
    }

    std::shared_ptr<const MeshData> decodeMeshPacket(uint8_t *rdram, uint32_t mesh)
    {
        std::vector<uint32_t> words;
        if (!readStream(rdram, mesh, words))
            return nullptr;
        // A mesh synced again since has another packet; one that was not is
        // the same MeshData, so the renderer keeps its buffers.
        uint32_t hash = 2166136261u;
        for (uint32_t word : words)
            hash = (hash ^ word) * 16777619u;
        Decoded &slot = s_decoded[mesh];
        if (slot.data && slot.hash == hash)
            return slot.data;

        auto out = std::make_shared<MeshData>();
        std::vector<std::array<uint32_t, 4>> vu(kVuQuads);
        uint32_t cycle = 1, write = 1;
        for (size_t i = 0; i < words.size();)
        {
            const uint32_t code = words[i++];
            const uint32_t command = (code >> 24) & 0x7fu;
            const uint32_t number = (code >> 16) & 0xffu;
            if (command == 0x00u) // NOP
                continue;
            if (command == 0x01u) // STCYCL
            {
                cycle = code & 0xffu;
                write = (code >> 8) & 0xffu;
                continue;
            }
            if (command == 0x14u || command == 0x17u) // MSCAL, MSCNT
            {
                if (!takeBatch(vu, *out))
                    return nullptr;
                continue;
            }
            if (command < 0x60u || write == 0u || cycle < write)
                return nullptr;
            // UNPACK: `write` quadwords of every `cycle`, from the address.
            const uint32_t parts = ((command >> 2) & 3u) + 1u;
            const uint32_t bits = 32u >> (command & 3u);
            const uint32_t count = number != 0u ? number : 256u;
            const uint32_t address = code & 0x3ffu;
            const size_t size = (parts * bits / 8u * count + 3u) / 4u;
            if (bits < 16u || i + size > words.size())
                return nullptr;
            const uint8_t *data = reinterpret_cast<const uint8_t *>(words.data() + i);
            i += size;
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t at = address + (k / write) * cycle + k % write;
                if (at >= kVuQuads)
                    return nullptr;
                for (uint32_t c = 0; c < parts; ++c)
                {
                    if (bits == 32u)
                        std::memcpy(&vu[at][c], data + (k * parts + c) * 4u, 4u);
                    else
                    {
                        int16_t value;
                        std::memcpy(&value, data + (k * parts + c) * 2u, 2u);
                        vu[at][c] = static_cast<uint32_t>(static_cast<int32_t>(value));
                    }
                }
            }
        }
        slot.hash = hash;
        slot.data = std::move(out);
        return slot.data;
    }
}
