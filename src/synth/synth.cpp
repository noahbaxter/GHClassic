// SYNTH_S, GH2's IOP synth module: the EE's side of the link.
//
// SynthEE batches its commands into one NOWAIT RPC per poll: {cmd, len, data}
// records, the data padded to 4 bytes and len left unpadded (0x22db38). The
// IOP answers the same way on the EE's own server, SID 0x75433179, whose
// RpcDispatch (0x22dcf0) walks the records under CritSec and hands each to
// the handler CtlServerInit stored (SynthEE::CtlDispatch). That server's
// sceSifRpcLoop never runs here, so replies are handed to the handler at the
// next CtlClientPoll instead: it runs inside SynthEE::Poll's CritSec (0x22cc94),
// which is the exclusion RpcDispatch would have taken. The module sends at
// the end of its 5 ms tick, so a reply arriving at the next poll is the
// latency the IOP had anyway.

#include "synth/synth.h"

#include "guest.h"
#include "hook.h"
#include "host/audio.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2x/iop/iop_subsystem.h"
#include "synth/module.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace gh2
{
    // The device is opened at the rate the SPU2 mixes at; nothing resamples.
    static_assert(kAudioRate == synth::Spu::kRate);

    namespace
    {
        // The SID SynthEE binds in CtlClientInit (0x22db68), looping until
        // the bind finds a server.
        constexpr uint32_t kSynthSid = 0x75433178u;

        const Addresses *s_addresses = nullptr;
        uint8_t *s_rdram = nullptr;
        synth::Module s_module;
        std::vector<synth::Reply> s_held;

        class Synth final : public ps2x::iop::IopService
        {
        public:
            [[nodiscard]] std::string_view name() const override
            {
                return "gh2 synth";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            // Dormant until the game loads SYNTH_S.IRX, as the real module is.
            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return kModuleAliases;
            }

            void reset() override
            {
                s_module.reset();
            }

            // CtlClientPoll (0x22dc68) is the only caller: function 0, NOWAIT,
            // no receive buffer and no end callback.
            [[nodiscard]] ps2x::iop::RpcResult handleRpc(const ps2x::iop::RpcRequest &request) override
            {
                ps2x::iop::RpcResult result;
                result.handled = request.sid == kSynthSid;
                result.resultAddress = request.receive.address;
                if (!result.handled || !s_rdram)
                    return result;
                uint32_t at = request.send.address;
                const uint32_t end = request.send.address + request.send.size;
                while (at + 8u <= end)
                {
                    const uint32_t cmd = load<uint32_t>(s_rdram, at);
                    const int32_t len = load<int32_t>(s_rdram, at + 4u);
                    const uint32_t data = at + 8u;
                    if (len < 0 || data + static_cast<uint32_t>(len) > end)
                        break;
                    s_module.command(cmd, getMemPtr(s_rdram, data), static_cast<uint32_t>(len));
                    at = (data + static_cast<uint32_t>(len) + 3u) & ~3u;
                }
                return result;
            }

        private:
            inline static constexpr std::array<uint32_t, 1> kSids{kSynthSid};
            inline static constexpr std::array<std::string_view, 1> kModuleAliases{"synth_s"};
        };

        // Each reply goes to the handler as RpcDispatch would pass it: cmd,
        // its data (null when empty) and its length. The data is staged in
        // the server's receive buffer, which nothing else fills, and each
        // call returns before the next is staged.
        void deliver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_module.takeReplies(s_held);
            const uint32_t handler = load<uint32_t>(rdram, s_addresses->synthServerHandler);
            if (handler == 0u)
                return; // the server is not up yet; they keep for the next poll
            std::vector<synth::Reply> replies;
            replies.swap(s_held);
            for (const synth::Reply &reply : replies)
            {
                uint32_t data = 0u;
                if (!reply.data.empty())
                {
                    data = s_addresses->synthServerBuffer;
                    std::memcpy(getMemPtr(rdram, data), reply.data.data(), reply.data.size());
                }
                runtime->callGuestFunction(rdram, ctx, handler,
                                           {reply.cmd, data, static_cast<uint32_t>(reply.data.size())});
            }
        }

        struct PollTag;
    }

    void installSynth(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_rdram = runtime.memory().getRDRAM();
        runtime.iop().addService(std::make_unique<Synth>());
        EntryHook<PollTag>::install(runtime, addresses.ctlClientPoll, deliver);
        setAudioSource(&synth::render);
    }

    void synth::render(float *out, size_t frames)
    {
        s_module.render(out, frames);
    }
}
