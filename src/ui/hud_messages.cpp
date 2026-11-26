// hud_messages: the HUD's messages (STAR POWER READY!, the note streaks,
// SONG FAILED) one at a time in each place they show. GH2's HUD has one
// place per player and one for the band (HudPanel::ShowMessage, 0x12c380),
// and starts a new message over whatever its place still shows, so two at
// once cut one short. Here each place keeps a queue: a message waits until
// the one before it has had its time. Face-Off's players have a place each;
// one player, and co-op's band, one.
//
// SONG FAILED goes ahead of everything, and what was waiting is dropped. GH2
// shows it in the band's place, wiping the players'.

#include "ui/hud_messages.h"

#include "guest.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <deque>

namespace gh2
{
    namespace
    {
        // HudPanel's messages: each player's, then the band's.
        constexpr uint32_t kPlayerMessages = 0x40u; // std::vector<HudMsg *>, begin and end
        constexpr uint32_t kBandMessage = 0x50u;    // HudMsg *
        // HudMsg (0x12c818): its TransAnim, and message_show_time, the
        // seconds Show plays that over (frames 0 to 100).
        constexpr uint32_t kAnim = 0x4u;    // RndTransAnim *
        constexpr uint32_t kSeconds = 0xcu; // float
        constexpr float kFrames = 100.0f;
        // RndTransAnim: whose keys it plays (itself, or another's), and those
        // keys, each a vector of 32-byte keys with their frame at +0x10.
        constexpr uint32_t kKeysOwner = 0x60u;
        constexpr uint32_t kKeyLists[] = {0x28u, 0x38u, 0x48u}; // turns, places, sizes
        constexpr int32_t kBand = -2;       // PlayerSetting kPlayerShared

        constexpr float kGap = 0.15f;

        struct Waiting
        {
            int32_t player;
            uint32_t symbol;
        };
        // Each player's, then the band's.
        std::array<std::deque<Waiting>, 3> s_waiting;
        std::array<float, 3> s_freeAt{};

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_original = nullptr;
        uint32_t s_songFailed = 0u;

        // The last frame a message's animation keys: past it, it's gone.
        float lastKey(uint8_t *rdram, uint32_t msg)
        {
            const uint32_t anim = load<uint32_t>(rdram, msg + kAnim);
            const uint32_t owner = anim ? load<uint32_t>(rdram, anim + kKeysOwner) : 0u;
            if (!owner)
                return kFrames;
            float last = 0.0f;
            for (const uint32_t list : kKeyLists)
            {
                const uint32_t begin = load<uint32_t>(rdram, owner + list);
                const uint32_t end = load<uint32_t>(rdram, owner + list + 4u);
                if (end > begin && (end - begin) % 32u == 0u)
                    last = std::max(last, load<float>(rdram, end - 32u + 0x10u));
            }
            return last > 0.0f ? std::min(last, kFrames) : kFrames;
        }

        // The song's seconds (TaskMgr's clocks at +0x28, now at +0xC), which
        // stop while paused, as the messages' own animations do.
        float now(uint8_t *rdram)
        {
            const uint32_t clocks = load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u);
            return clocks ? load<float>(rdram, clocks + 0xcu) : 0.0f;
        }

        // A HUD with no band's message (one player's) shows the band's in
        // player 1's, as ShowMessage does, so they share a queue.
        bool hasBand(uint8_t *rdram, uint32_t hud)
        {
            return load<uint32_t>(rdram, hud + kBandMessage) != 0u;
        }

        size_t place(uint8_t *rdram, uint32_t hud, int32_t player)
        {
            if (player == kBand)
                return hasBand(rdram, hud) ? 2u : 0u;
            return static_cast<size_t>(player);
        }

        uint32_t message(uint8_t *rdram, uint32_t hud, int32_t player)
        {
            if (player == kBand && hasBand(rdram, hud))
                return load<uint32_t>(rdram, hud + kBandMessage);
            if (player == kBand)
                player = 0;
            const uint32_t begin = load<uint32_t>(rdram, hud + kPlayerMessages);
            const uint32_t end = load<uint32_t>(rdram, hud + kPlayerMessages + 4u);
            const uint32_t at = begin + 4u * static_cast<uint32_t>(player);
            return player >= 0 && at < end ? load<uint32_t>(rdram, at) : 0u;
        }

        // GH2's own ShowMessage, now; its place busy until it has gone.
        void show(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hud, int32_t player, uint32_t symbol)
        {
            const uint32_t msg = message(rdram, hud, player);
            if (!msg)
                return;
            const float until = now(rdram) + lastKey(rdram, msg) * load<float>(rdram, msg + kSeconds) / kFrames + kGap;
            if (symbol == s_songFailed)
                s_freeAt.fill(until);
            else
                s_freeAt[place(rdram, hud, player)] = until;
            runtime->callGuestFunction(rdram, ctx, s_addresses->hudPanelShowMessage,
                                       {hud, static_cast<uint32_t>(player), symbol}, s_original);
        }

        // HudPanel::ShowMessage(this, PlayerSetting, Symbol).
        void onShowMessage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t hud = GPR_U32(ctx, 4);
            const int32_t player = static_cast<int32_t>(GPR_U32(ctx, 5));
            const uint32_t symbol = GPR_U32(ctx, 6);
            if (player != kBand && (player < 0 || player > 1))
            {
                s_original(rdram, ctx, runtime);
                return;
            }
            if (symbol == s_songFailed)
            {
                for (auto &waiting : s_waiting)
                    waiting.clear();
                show(rdram, ctx, runtime, hud, player, symbol);
            }
            else
            {
                const size_t at = place(rdram, hud, player);
                if (now(rdram) < s_freeAt[at] || !s_waiting[at].empty())
                    s_waiting[at].push_back({player, symbol});
                else
                    show(rdram, ctx, runtime, hud, player, symbol);
            }
            leave(ctx);
        }

        // Each place's next message, once the one before has gone.
        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t hud = GPR_U32(ctx, 4);
            for (size_t at = 0; at < s_waiting.size(); ++at)
                if (!s_waiting[at].empty() && now(rdram) >= s_freeAt[at])
                {
                    const Waiting next = s_waiting[at].front();
                    s_waiting[at].pop_front();
                    show(rdram, ctx, runtime, hud, next.player, next.symbol);
                }
            args.restore(ctx);
        }

        // A song starting or starting over: nothing waits.
        struct ResetTag;
        void onReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            if (!s_songFailed)
                s_songFailed = script::symbol(rdram, ctx, runtime, "song_failed");
            for (auto &waiting : s_waiting)
                waiting.clear();
            s_freeAt.fill(0.0f);
            args.restore(ctx);
        }
    }

    void installHudMessages(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        replaceKeeping(runtime, addresses.hudPanelShowMessage, &onShowMessage, s_original);
        EntryHook<PollTag>::install(runtime, addresses.hudPanelPoll, onPoll);
        EntryHook<ResetTag>::install(runtime, addresses.hudPanelReset, onReset);
    }
}
