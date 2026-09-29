// SYNTH_S, GH2's IOP synth module, with nothing played but every reply the
// game waits on sent.
//
// The EE's SynthEE batches its synth commands into one NOWAIT RPC per poll:
// packed {cmd, len, data} records, each padded to 4 bytes (0x22db08). The
// IOP answers the same way on the EE's own server, SID 0x75433179, whose
// RpcDispatch (0x22dcf0) walks the records under CritSec and hands each to
// the handler CtlServerInit stored (SynthEE::CtlDispatch). That server's
// sceSifRpcLoop never runs here, so replies are handed to the handler at the
// next CtlClientPoll instead: it runs inside SynthEE::Poll's CritSec (0x22cc94),
// which is the exclusion RpcDispatch would have taken, and a reply arriving
// a poll later is what the IOP's latency looked like anyway.
//
// Answered: 0x190 stream info with reply 2, which readies the stream
// (StreamEE::Poll sets state 3, 0x230960); 0xc9 SPU chunk with 13, which
// clears SPUSendPoll's in-flight flag; 0x1 terminate with 14. The song clock
// is StreamEE's own VarTimer, started by Play (0x230d6c), so no position
// (reply 3) or data request (reply 0) is needed for a song to run.
//
// Not answered yet: 11, a one-shot sample finishing, so a SampleInstEE
// started here never reports done.

#include "silent_synth.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2x/iop/iop_subsystem.h"

#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace gh2
{
    namespace
    {
        // The SID SynthEE binds in CtlClientInit (retail 0x22db68), looping
        // until the bind finds a server.
        constexpr uint32_t kSynthSid = 0x75433178u;

        // Commands the EE sends and the replies they get (CtlDispatch_impl's
        // jump table, 0x466540).
        constexpr uint32_t kCmdTerminate = 0x1u;
        constexpr uint32_t kCmdSpuChunk = 0xc9u;
        constexpr uint32_t kCmdStreamInfo = 0x190u;
        constexpr uint32_t kReplyStreamOp = 2u;   // StreamEE::Dispatch(id, 2): ready
        constexpr uint32_t kReplySpuDone = 13u;   // SPUSetSendDone
        constexpr uint32_t kReplyTerminated = 14u;

        struct Reply
        {
            uint32_t cmd = 0;
            std::vector<uint8_t> data;
        };

        const Addresses *s_addresses = nullptr;
        uint8_t *s_rdram = nullptr;
        std::mutex s_mutex;
        std::vector<Reply> s_pending;

        void queue(uint32_t cmd, std::vector<uint8_t> data = {})
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_pending.push_back({cmd, std::move(data)});
        }

        class SilentSynth final : public ps2x::iop::IopService
        {
        public:
            [[nodiscard]] std::string_view name() const override
            {
                return "gh2 silent synth";
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
                std::lock_guard<std::mutex> lock(s_mutex);
                s_pending.clear();
            }

            // CtlClientPoll (0x22dc68) is the only caller: function 0, NOWAIT,
            // no receive buffer and no end callback.
            [[nodiscard]] ps2x::iop::RpcResult handleRpc(const ps2x::iop::RpcRequest &request) override
            {
                ps2x::iop::RpcResult result;
                result.handled = request.sid == kSynthSid;
                result.resultAddress = request.receive.address;
                if (result.handled && s_rdram)
                    readBatch(request.send);
                return result;
            }

        private:
            static void readBatch(const ps2x::iop::GuestBuffer &send)
            {
                uint32_t at = send.address;
                const uint32_t end = send.address + send.size;
                while (at + 8u <= end)
                {
                    const uint32_t cmd = load<uint32_t>(s_rdram, at);
                    const int32_t len = load<int32_t>(s_rdram, at + 4u);
                    const uint32_t data = at + 8u;
                    at = data + static_cast<uint32_t>(len > 0 ? len : 0);
                    at = (at + 3u) & ~3u;
                    if (cmd == kCmdStreamInfo && len >= 4)
                    {
                        // The stream's id (StreamEE +0x28) is the payload's first word.
                        std::vector<uint8_t> id(4);
                        std::memcpy(id.data(), getMemPtr(s_rdram, data), 4);
                        queue(kReplyStreamOp, std::move(id));
                    }
                    else if (cmd == kCmdSpuChunk)
                        queue(kReplySpuDone);
                    else if (cmd == kCmdTerminate)
                        queue(kReplyTerminated);
                }
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kSynthSid};
            inline static constexpr std::array<std::string_view, 1> kModuleAliases{"synth_s"};
        };

        // Each reply goes to the handler as RpcDispatch would pass it: cmd,
        // its data (null when empty) and its length. The data is staged in
        // the server's receive buffer, which nothing else fills.
        void deliver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            std::vector<Reply> replies;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                replies.swap(s_pending);
            }
            const uint32_t handler = load<uint32_t>(rdram, s_addresses->synthServerHandler);
            if (handler == 0u)
            {
                // The server is not up yet; keep them for the next poll.
                std::lock_guard<std::mutex> lock(s_mutex);
                s_pending.insert(s_pending.begin(), replies.begin(), replies.end());
                return;
            }
            for (const Reply &reply : replies)
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

    void installSilentSynth(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_rdram = runtime.memory().getRDRAM();
        runtime.iop().addService(std::make_unique<SilentSynth>());
        EntryHook<PollTag>::install(runtime, addresses.ctlClientPoll, deliver);
    }
}
