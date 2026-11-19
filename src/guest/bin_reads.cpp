#include "guest/bin_reads.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/host_clock.h"

#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace gh2::bin_reads
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // BinStream's: nonzero when the stream's bytes are in the PS2's
        // order, else ReadEndian swaps each value.
        constexpr uint32_t kLittleEndian = 0x0u;

        // CharBonesSamples': whether quats and rotations are halved, the
        // offsets in a sample its pointers are set to (SetPointers, 0x167d98:
        // ten, from +0x3c, to +0x68), a sample's size, how many, and where.
        constexpr uint32_t kCompressed = 0x0u, kOffsets = 0x3cu, kPointers = 0x68u, kSampleSize = 0x64u,
                           kSamples = 0x94u, kData = 0x98u;

        // Native work charged to the guest's clock at what it took, less the
        // cycles the guest code it called counted: the loader budgets each
        // frame by that clock (LoadMgr::Poll's COP0 Count), and work that
        // cost no cycles had it load on long past its frame. Not on a
        // stepped clock (a recording's, runtime/host_clock.h), whose frames
        // take what they take: charged by the host's time, the same load
        // would land on another frame on a busier machine.
        struct Charged
        {
            EeScheduler &scheduler;
            const uint32_t count = scheduler.readCount();
            const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

            ~Charged()
            {
                if (ps2x::host_clock::stepped())
                    return;
                const auto took = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
                const uint64_t cycles = uint64_t(took.count()) * EeScheduler::kEeClockHz / 1000000000ull;
                const uint32_t counted = scheduler.readCount() - count;
                if (cycles > counted)
                    scheduler.accountCycles(static_cast<uint32_t>(cycles - counted));
            }
        };

        void swap16(uint8_t *p, size_t bytes)
        {
            for (size_t i = 0; i + 1u < bytes; i += 2u)
                std::swap(p[i], p[i + 1u]);
        }

        void swap32(uint8_t *p, size_t bytes)
        {
            for (size_t i = 0; i + 3u < bytes; i += 4u)
            {
                std::swap(p[i], p[i + 3u]);
                std::swap(p[i + 1u], p[i + 2u]);
            }
        }

        // CharBonesSamples::LoadData (0x194978): per sample, its positions
        // (to the third offset, 12 bytes read into each 16), then its quats
        // (to the fourth, four halves or words each) and rotations (to the
        // tenth, a half or a word each), all one run in the file and the
        // quats and rotations one in memory. Read here in one, at where the
        // quats start less the positions' file bytes, and the positions
        // spread to their 16 after, each slot's last word as it was.
        struct LoadDataTag;
        bool loadData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4), stream = GPR_U32(ctx, 5);
            const int32_t samples = load<int32_t>(rdram, self + kSamples);
            uint32_t offsets[10];
            for (uint32_t k = 0; k < 10u; ++k)
                offsets[k] = load<uint32_t>(rdram, self + kOffsets + 4u * k);
            const bool compressed = load<uint32_t>(rdram, self + kCompressed) != 0u;
            const uint32_t pos = offsets[0], quats = offsets[2], rots = offsets[3], end = offsets[9];
            // Only the layout the game's own loops step through whole.
            if (samples <= 0 || quats < pos || rots < quats || end < rots || (quats - pos) % 16u != 0u ||
                (rots - quats) % (compressed ? 8u : 16u) != 0u || (end - rots) % (compressed ? 2u : 4u) != 0u)
                return false;
            const uint32_t positions = (quats - pos) / 16u;
            const uint32_t bytes = positions * 12u + (end - quats);
            const uint32_t data = load<uint32_t>(rdram, self + kData), size = load<uint32_t>(rdram, self + kSampleSize);
            const bool swap = load<uint32_t>(rdram, stream + kLittleEndian) == 0u;
            const Charged charged{runtime->eeScheduler()};
            std::vector<uint32_t> pads(positions);
            const EntryArgs args(ctx);
            for (int32_t i = 0; i < samples; ++i)
            {
                const uint32_t base = data + size * static_cast<uint32_t>(i);
                for (uint32_t k = 0; k < positions; ++k)
                    pads[k] = load<uint32_t>(rdram, base + pos + 16u * k + 12u);
                if (bytes != 0u)
                    runtime->callGuestFunction(rdram, ctx, s_addresses->binStreamRead,
                                               {stream, base + pos + 4u * positions, bytes});
                uint8_t *p = getMemPtr(rdram, base + pos);
                // Toward the start, so a slot never covers a value not yet moved.
                for (uint32_t k = 0; k < positions; ++k)
                {
                    std::memmove(p + 16u * k, p + 4u * positions + 12u * k, 12u);
                    std::memcpy(p + 16u * k + 12u, &pads[k], 4u);
                }
                if (swap)
                {
                    for (uint32_t k = 0; k < positions; ++k)
                        swap32(p + 16u * k, 12u);
                    if (compressed)
                        swap16(p + (quats - pos), end - quats);
                    else
                        swap32(p + (quats - pos), end - quats);
                }
            }
            // The pointers as SetPointers left them, at the last sample.
            const uint32_t last = data + size * static_cast<uint32_t>(samples - 1);
            for (uint32_t k = 0; k < 10u; ++k)
                store<uint32_t>(rdram, self + kPointers + 4u * k, last + offsets[k]);
            args.restore(ctx);
            leave(ctx);
            return true;
        }

        // operator>>(BinStream &, RndMesh::Vert &) (0x1c8328): its position
        // (+0x0), two weights (+0x38) for a revision under 23 but 10, its
        // normal (+0x10), colour (+0x20) and uv (+0x30), and two words a
        // revision under 11 drops, each a word read on its own. Read here in
        // one, each put in place, the vert's other words as they were.
        struct ReadVertTag;
        bool readVert(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t stream = GPR_U32(ctx, 4), vert = GPR_U32(ctx, 5);
            const int32_t rev = load<int32_t>(rdram, s_addresses->rndMeshRev);
            const bool weights = rev != 10 && rev < 23, dropped = rev < 11;
            std::vector<std::pair<uint32_t, uint32_t>> fields{{0x0u, 12u}};
            if (weights)
                fields.push_back({0x38u, 8u});
            fields.insert(fields.end(), {{0x10u, 12u}, {0x20u, 16u}, {0x30u, 8u}});
            uint32_t bytes = dropped ? 8u : 0u;
            for (const auto &f : fields)
                bytes += f.second;
            // Into the vert's own 64 bytes, put back after.
            const Charged charged{runtime->eeScheduler()};
            uint8_t kept[64], read[64];
            std::memcpy(kept, getMemPtr(rdram, vert), sizeof(kept));
            const EntryArgs args(ctx);
            runtime->callGuestFunction(rdram, ctx, s_addresses->binStreamRead, {stream, vert, bytes});
            args.restore(ctx);
            std::memcpy(read, getMemPtr(rdram, vert), bytes);
            std::memcpy(getMemPtr(rdram, vert), kept, sizeof(kept));
            if (load<uint32_t>(rdram, stream + kLittleEndian) == 0u)
                swap32(read, bytes);
            uint32_t at = 0u;
            for (const auto &f : fields)
            {
                std::memcpy(getMemPtr(rdram, vert + f.first), read + at, f.second);
                at += f.second;
            }
            SET_GPR_U32(ctx, 2, stream);
            leave(ctx);
            return true;
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        Override<LoadDataTag>::install(runtime, addresses.charBonesSamplesLoadData, loadData);
        Override<ReadVertTag>::install(runtime, addresses.rndMeshReadVert, readVert);
    }
}
