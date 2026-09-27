// PsRnd, GH2's PS2 renderer backend, with nothing drawn. Every Ps* class
// still builds packets into the one DmaPacket; this keeps that packet's
// bookkeeping exactly as retail leaves it and never hands a packet to DMA.

#include "null_rnd.h"

#include "guest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

namespace gh2
{
    namespace
    {
        // The engine's one DmaPacket sits at the start of scratchpad:
        // +0 open DMA tag, +4 buffer base, +8 write cursor, +0xC open GIF tag,
        // +0x10 GIF tag state, +0x14 the flag DmaPacket::Set stores. Its
        // buffer alternates between two halves of scratchpad.
        constexpr uint32_t kPacket = 0x70000000u;
        constexpr uint32_t kHalfA = 0x70000020u;
        constexpr uint32_t kHalfB = 0x70002010u;
        constexpr uint32_t kQword = 16u;

        // PsRnd::FlushPacket, retail 0x3d7d58. Retail closes the DMA tag,
        // waits on channel 8, spins until the MFIFO ring has room, sends the
        // packet, then reopens a tag in the other half. Only the effect on the
        // packet is kept:
        //  - closing a tag with nothing after it takes the tag back
        //    (CloseDmaTag 0x3d80c8), so an empty packet stays where it is;
        //  - a packet with content moves to the other half, through
        //    DmaPacket::Set and Reset (0x19bdc0, 0x19bde8), which zero both
        //    GIF fields CloseGifTag would otherwise have closed;
        //  - OpenDmaTag (0x3d8088) opens the next tag at the cursor.
        void flushPacket(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t tag = load<uint32_t>(rdram, kPacket + 0x0u);
            uint32_t base = load<uint32_t>(rdram, kPacket + 0x4u);
            uint32_t cursor = load<uint32_t>(rdram, kPacket + 0x8u);

            if (tag != 0u && tag == cursor - kQword)
            {
                cursor = tag;
            }

            if (cursor != base)
            {
                base = base == kHalfA ? kHalfB : kHalfA;
                cursor = base;
                store<uint32_t>(rdram, kPacket + 0x4u, base);
                store<uint32_t>(rdram, kPacket + 0xCu, 0u);
                store<uint32_t>(rdram, kPacket + 0x10u, 0u);
                store<uint32_t>(rdram, kPacket + 0x14u, 1u);
            }

            store<uint32_t>(rdram, kPacket + 0x0u, cursor);
            store<uint32_t>(rdram, kPacket + 0x8u, cursor + kQword);

            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installNullRnd(PS2Runtime &runtime, const Addresses &addresses)
    {
        runtime.replaceFunction(addresses.psRndFlushPacket, flushPacket);
    }
}
