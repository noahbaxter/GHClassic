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
#include "synth/adpcm.h"
#include "synth/module.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
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

        constexpr uint32_t kReplyStreamPosition = 3u; // {id, lines played}

        const Addresses *s_addresses = nullptr;
        uint8_t *s_rdram = nullptr;
        synth::Module s_module;
        std::vector<synth::Reply> s_held;
        // When each stream's last position was taken, by stream id.
        std::map<uint32_t, ps2x::host_clock::Clock::time_point> s_positionAt;

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
                if (reply.cmd == kReplyStreamPosition && reply.data.size() >= 4u)
                {
                    uint32_t id;
                    std::memcpy(&id, reply.data.data(), 4u);
                    s_positionAt[id] = reply.at;
                }
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

        // SPUStartSend(data, size, dest), retail 0x231a78, queues an upload
        // that SPUSendPoll sends 0x5000 bytes a poll, each answered before
        // the next, and SynthSamplePs::SynthPoll (0x22da78) starts one sample
        // a frame. Written at once, nothing is pending, so SynthPoll finishes
        // each sample in the call that starts it.
        void spuStartSend(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            s_module.writeRam(GPR_U32(ctx, 6), getMemPtr(rdram, GPR_U32(ctx, 4)), GPR_U32(ctx, 5));
            ctx->pc = GPR_U32(ctx, 31);
        }

        struct PollTag;

        // StreamEE::Poll (0x2309dc) holds its VarTimer (+0x130), the song
        // clock, to the IOP's played position (+0x170, lines) only past
        // 100 ms of drift. Under that it free-runs, so a hitch that starves
        // the audio for less than 100 ms leaves the clock ahead of the music
        // for the rest of the song, and the snap itself lands on a position a
        // tick and a poll old. Here, before that check: the clock is pulled
        // to the position plus its age at up to 3% of time, past 50 ms
        // snapped there at once, and the position is consumed so the check
        // never fires. Nothing is pulled from a position more than 50 ms old
        // (one from before a hitch: the audio may have starved since), nor
        // until the clock and host time have kept together for 100 ms (the
        // EE stands still in a hitch, then catches up in bursts). The VarTimer
        // reads base (+0x30) plus elapsed time times rate (+0x34), so moving
        // the base moves the clock and nothing else.
        constexpr uint32_t kStreamId = 0x28u, kStreamState = 0x4cu, kStreamPlaying = 4u;
        constexpr uint32_t kStartMs = 0x124u, kTimer = 0x130u, kPlayed = 0x170u;
        constexpr uint32_t kTimerBase = 0x30u, kTimerRate = 0x34u;
        constexpr float kSnapMs = 50.0f;
        constexpr float kSlew = 0.03f;    // of time passed
        constexpr float kSettleS = 0.5f;  // the time an error takes to be pulled in, under the slew
        constexpr float kStaleMs = 50.0f; // a position older (one from before a hitch) is dropped
        constexpr float kStepMs = 20.0f;  // the clock and host time apart by more since the last report: the EE caught up
        constexpr auto kSteadyFor = std::chrono::milliseconds(100); // together that long before any pull

        // A playing stream's clock at the last report, after its pull.
        struct Lock
        {
            ps2x::host_clock::Clock::time_point at;
            float clockMs;
            ps2x::host_clock::Clock::time_point steadySince; // the clock and host time together since
        };
        std::map<uint32_t, Lock> s_locks; // by StreamEE

        struct StreamPollTag;
        void lockClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            if (load<uint32_t>(rdram, self + kStreamState) != kStreamPlaying)
            {
                s_locks.erase(self);
                return;
            }
            const int32_t played = load<int32_t>(rdram, self + kPlayed);
            if (played < 0)
                return;
            const auto at = s_positionAt.find(load<uint32_t>(rdram, self + kStreamId));
            if (at == s_positionAt.end())
                return;

            const uint32_t rate =
                static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->streamSampleFreq, {self, 0u}));
            runtime->callGuestFunction(rdram, ctx, s_addresses->varTimerMs, {self + kTimer});
            const float clockMs = ctx->f[0];
            SET_GPR_U32(ctx, 4, self);
            if (rate == 0u)
                return;

            const auto now = ps2x::host_clock::now();
            const float timerRate = load<float>(rdram, self + kTimer + kTimerRate);
            const float ageMs = std::chrono::duration<float, std::milli>(now - at->second).count() * timerRate;
            const float audioMs = static_cast<float>(played) * synth::kAdpcmBlockSamples * 1000.0f / static_cast<float>(rate) +
                                  load<float>(rdram, self + kStartMs) + ageMs;
            const float error = clockMs - audioMs;
            store<int32_t>(rdram, self + kPlayed, -1);

            const auto [lock, first] = s_locks.try_emplace(self, Lock{now, clockMs, now});
            const float dtMs = std::chrono::duration<float, std::milli>(now - lock->second.at).count() * timerRate;
            const float dClockMs = clockMs - lock->second.clockMs;
            if (std::fabs(dClockMs - dtMs) > kStepMs)
                lock->second.steadySince = now;
            const bool steady = first || now - lock->second.steadySince >= kSteadyFor;
            float pull = 0.0f;
            if (ageMs <= kStaleMs && steady)
            {
                pull = error;
                if (std::fabs(error) <= kSnapMs && !first)
                {
                    const float limit = kSlew * dtMs;
                    pull = std::clamp(error * std::min(1.0f, dtMs / (kSettleS * 1000.0f)), -limit, limit);
                }
            }
            lock->second.at = now;
            lock->second.clockMs = clockMs - pull;
            const uint32_t base = self + kTimer + kTimerBase;
            store<float>(rdram, base, load<float>(rdram, base) - pull);
        }
    }

    void installSynth(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_rdram = runtime.memory().getRDRAM();
        runtime.iop().addService(std::make_unique<Synth>());
        EntryHook<PollTag>::install(runtime, addresses.ctlClientPoll, deliver);
        EntryHook<StreamPollTag>::install(runtime, addresses.streamEEPoll, lockClock);
        runtime.replaceFunction(addresses.spuStartSend, spuStartSend);
        setAudioSource(&synth::render);
    }

    void synth::render(float *out, size_t frames)
    {
        s_module.render(out, frames);
    }
}
