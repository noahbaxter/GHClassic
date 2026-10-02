// Menu music: fades carried across screen changes, and each loop played
// through into the next instead of over again.
//
// MetaMusic::Poll (0x21f180) fades its stream in over one second on a
// LinearInterpolator at +0x80, keyed on TaskMgr::UISeconds * 1000: Reset
// (-96 dB, target, now, now + 1000 / rate) at 0x21f378, then each poll the
// volume is slope * now + intercept, clamped to [-96 dB, target]. Every
// screen change zeroes UISeconds. On a PS2 the fade starts after the first
// one: ReadyToPlay (0x21f138) waits for 200 KB of the stream, which the drive
// takes longer to deliver than the switch to bootup_load. Disc reads here are
// instant, the fade starts first, and after the reset its window lies
// seconds ahead, so the boot music sits at -96 dB until it is replaced.
//
// So when the UI clock goes back between polls, the fade's window goes back
// with it and the fade carries on from where it was.
//
// MetaPanel::Load (0x134608) plays sfx/streams/<name>, a name from (synth
// metamusic music) picked by PickLoopIndex (0x1349d8), and MetaMusic::Start
// (0x21f478) loops it with SetJump(kStreamEnd, 0, NULL). A jump given a file
// switches to it: VAGFileReader::SetJump (0x231f30) opens <file>.vgs, DoJump
// (0x232af0) swaps it in at the jump. So whenever the stream's reader has no
// file to jump to, it gets the next pick. All nine loops are 22050 Hz stereo,
// which DoJump needs, as it keeps the first file's header.
//
// PickLoopIndex only steers clear of the last three, fine for one pick per
// visit to the menus but not for playing on, so it deals from a shuffled
// deck of all of them instead, on the game's Rand, never repeating across a
// reshuffle.

#include "ui/meta_music.h"

#include "guest.h"
#include "hook.h"
#include "ps2_runtime_macros.h"

#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace gh2
{
    namespace
    {
        // LinearInterpolator, from Reset (0x2d96b8).
        constexpr uint32_t kFade = 0x80u;
        constexpr uint32_t kX0 = 0x08u, kX1 = 0x0cu, kSlope = 0x14u, kIntercept = 0x18u;

        const Addresses *s_addresses = nullptr;
        // The UI clock at the last poll, of the one MetaMusic (MetaPanel's).
        uint32_t s_polled = 0u;
        float s_lastUi = 0.0f;

        // TaskMgr::UISeconds (0x2c6750).
        float uiSeconds(uint8_t *rdram)
        {
            return load<float>(rdram, load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u) + 0x34u);
        }

        void add(uint8_t *rdram, uint32_t address, float delta)
        {
            store<float>(rdram, address, load<float>(rdram, address) + delta);
        }

        struct PollTag;

        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const float now = uiSeconds(rdram);
            const bool first = self != s_polled;
            const float back = s_lastUi - now;
            s_polled = self;
            s_lastUi = now;
            if (first || back <= 0.0f)
                return;
            const uint32_t fade = self + kFade;
            const float ms = back * 1000.0f;
            add(rdram, fade + kX0, -ms);
            add(rdram, fade + kX1, -ms);
            add(rdram, fade + kIntercept, load<float>(rdram, fade + kSlope) * ms);
        }

        constexpr uint32_t kMusic = 0x64u; // MetaPanel's MetaMusic
        constexpr uint32_t kStream = 0x28u, kPlaying = 0x2cu, kRampingDown = 0x34u, kLoop = 0x40u;
        constexpr uint32_t kSetJump = 0xd0u; // Stream vtable slot, as Start calls it
        constexpr uint32_t kReader = 0x6cu;  // StreamEE's VAGFileReader
        constexpr uint32_t kJumpPending = 0x2cu, kJumpFile = 0x38u;

        bool s_chainFailed = false;

        std::vector<uint32_t> s_deck; // indices into (music ...), dealt from the back
        uint32_t s_deckSize = 0u, s_dealt = 0u;

        // The next of indices 1 to size - 1, (music ...)'s names.
        uint32_t deal(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t size)
        {
            if (size != s_deckSize)
            {
                s_deck.clear();
                s_deckSize = size;
                s_dealt = 0u;
            }
            if (s_deck.empty())
            {
                for (uint32_t i = size - 1u; i >= 1u; --i)
                    s_deck.push_back(i);
                for (uint32_t i = static_cast<uint32_t>(s_deck.size()) - 1u; i >= 1u; --i)
                    std::swap(s_deck[i], s_deck[runtime->callGuestFunction(rdram, ctx, s_addresses->randomInt, {0u, i + 1u})]);
                if (s_deck.size() > 1u && s_deck.back() == s_dealt)
                    std::swap(s_deck.back(), s_deck.front());
            }
            s_dealt = s_deck.back();
            s_deck.pop_back();
            return s_dealt;
        }

        // MetaPanel::PickLoopIndex(this, size).
        void pickLoopIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t index = deal(rdram, ctx, runtime, GPR_U32(ctx, 5));
            SET_GPR_U32(ctx, 2, index);
            ctx->pc = returnTo;
        }

        struct PanelPollTag;

        void onPanelPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            const uint32_t music = load<uint32_t>(rdram, panel + kMusic);
            if (s_chainFailed || music == 0u || !load<uint32_t>(rdram, music + kPlaying) ||
                !load<uint32_t>(rdram, music + kLoop) || load<uint32_t>(rdram, music + kRampingDown))
                return;
            const uint32_t stream = load<uint32_t>(rdram, music + kStream);
            if (stream == 0u)
                return;
            const uint32_t vtable = load<uint32_t>(rdram, stream + 4u);
            const uint32_t self = stream + load<int16_t>(rdram, vtable + kSetJump);
            const uint32_t reader = load<uint32_t>(rdram, self + kReader);
            if (reader == 0u || (load<uint32_t>(rdram, reader + kJumpPending) && load<uint32_t>(rdram, reader + kJumpFile)))
                return;

            const auto call = [&](uint32_t function, std::initializer_list<uint32_t> in)
            { return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, in)); };
            // `text` on the game's heap, after `prefix` bytes.
            const auto guestString = [&](const std::string &text, uint32_t prefix)
            {
                const uint32_t block = call(s_addresses->builtinNew, {prefix + static_cast<uint32_t>(text.size()) + 1u});
                std::memcpy(getMemPtr(rdram, block + prefix), text.c_str(), text.size() + 1u);
                return block;
            };
            const auto symbol = [&](const char *text)
            {
                const uint32_t block = guestString(text, 4u);
                call(s_addresses->symbolCtor, {block, block + 4u});
                const uint32_t interned = load<uint32_t>(rdram, block);
                call(s_addresses->builtinDelete, {block});
                return interned;
            };

            const uint32_t list = call(s_addresses->systemConfig3, {symbol("synth"), symbol("metamusic"), symbol("music")});
            const uint32_t size = static_cast<uint32_t>(load<int16_t>(rdram, list + 8u));
            const uint32_t pick = deal(rdram, ctx, runtime, size);
            const uint32_t name = load<uint32_t>(rdram, load<uint32_t>(rdram, list) + 8u * pick);
            const std::string path = "sfx/streams/" + std::string(reinterpret_cast<const char *>(getMemPtr(rdram, name)));

            const uint32_t file = guestString(path, 0u);
            ctx->f[12] = load<float>(rdram, s_addresses->streamEndMs);
            ctx->f[13] = 0.0f;
            call(load<uint32_t>(rdram, vtable + kSetJump + 4u), {self, file});
            call(s_addresses->builtinDelete, {file});
            SET_GPR_U32(ctx, 4, panel); // Poll's only argument

            if (load<uint32_t>(rdram, reader + kJumpFile) == 0u)
            {
                std::cerr << "[meta_music] couldn't open " << path << ".vgs, looping instead" << std::endl;
                s_chainFailed = true;
            }
        }
    }

    void installMetaMusic(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<PollTag>::install(runtime, addresses.metaMusicPoll, onPoll);
        EntryHook<PanelPollTag>::install(runtime, addresses.metaPanelPoll, onPanelPoll);
        runtime.replaceFunction(addresses.metaPanelPickLoopIndex, pickLoopIndex);
    }
}
