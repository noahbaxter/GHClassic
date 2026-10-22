#include "dev/scripted_play.h"

#include "guest/beatmatch.h"
#include "guest/data_array.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace gh2::scripted_play
{
    namespace
    {
        using guest::BeatMatcher;
        using guest::GameGemList;
        using guest::GuitarTrackWatcherImpl;

        enum class Kind
        {
            kStrum,   // every fret let go, `slots` pressed, a strum
            kPress,   // `slots` pressed
            kRelease, // every fret let go
            kHammer,  // `slots` hammered on, or pulled off to from the frets above
            kMark,    // $var set to player 0's score or streak
        };

        struct Action
        {
            float at;
            Kind kind;
            uint32_t slots = 0u;
            std::string var;
            bool streak = false;
        };

        const Addresses *s_addresses = nullptr;
        uint32_t s_matcher = 0u;   // player 0's BeatMatcher, while it has a guitar track
        uint32_t s_held = 0u;      // frets held, a bit each, as GuitarController's ctrlHeld
        float s_last = 0.0f;       // the time at the last poll
        float s_frame = 0.0f;      // and how far it moved
        float s_worst = 0.0f;      // the furthest a strum or fret press has gone in from its time, this song
        std::vector<Action> s_queue; // by time, those at one time in the order given

        GuitarTrackWatcherImpl watcher(uint8_t *rdram)
        {
            return GuitarTrackWatcherImpl(rdram, s_matcher ? BeatMatcher(rdram, s_matcher).swingWatcher() : 0u);
        }

        // GuitarController::GetCurrentSlot (0x236ba0): the highest fret held, or -1.
        int highest(uint32_t held)
        {
            return held ? 31 - std::countl_zero(held) : -1;
        }

        void queue(Action action)
        {
            const auto at = std::upper_bound(s_queue.begin(), s_queue.end(), action.at,
                                             [](float t, const Action &a) { return t < a.at; });
            s_queue.insert(at, std::move(action));
        }

        struct Pad
        {
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *runtime;

            void call(uint32_t function, int32_t slot, uint32_t flag = 0u) const
            {
                runtime->callGuestFunction(rdram, ctx, function, {s_matcher, static_cast<uint32_t>(slot), flag});
            }

            void down(int slot) const
            {
                call(s_addresses->beatMatcherFretButtonDown, slot);
                if ((s_held >> slot) == 0u)
                    call(s_addresses->beatMatcherNonStrumSwing, slot, 1u);
                s_held |= 1u << slot;
            }

            void up(int slot) const
            {
                call(s_addresses->beatMatcherFretButtonUp, slot);
                s_held &= ~(1u << slot);
                if (s_held != 0u && highest(s_held) < slot)
                    call(s_addresses->beatMatcherNonStrumSwing, highest(s_held), 0u);
            }

            // Lowest first, so a press or a letting go is never a pull-off
            // or a hammer-on but on the highest fret.
            void press(uint32_t slots) const
            {
                for (int slot = 0; slot < 5; ++slot)
                    if ((slots >> slot) & 1u && !((s_held >> slot) & 1u))
                        down(slot);
            }

            void release() const
            {
                for (int slot = 0; slot < 5; ++slot)
                    if ((s_held >> slot) & 1u)
                        up(slot);
            }

            // A press above every fret held is a hammer-on; else the fret
            // goes down first and those above it come up, the last of them
            // a pull-off.
            void hammer(uint32_t slots) const
            {
                const int slot = highest(slots);
                const bool on = highest(s_held) < slot;
                press(slots);
                if (!on)
                    for (int above = 4; above > slot; --above)
                        if ((s_held >> above) & 1u)
                            up(above);
            }

            void strum() const { call(s_addresses->beatMatcherSwing, highest(s_held), 1u); }
        };

        void fire(const Pad &pad, const Action &action, float now)
        {
            const char *what = "";
            switch (action.kind)
            {
            case Kind::kStrum:
                what = "strum";
                pad.release();
                pad.press(action.slots);
                pad.strum();
                break;
            case Kind::kPress:
                what = "press";
                pad.press(action.slots);
                break;
            case Kind::kRelease:
                what = "release";
                pad.release();
                break;
            case Kind::kHammer:
                what = "hammer";
                pad.hammer(action.slots);
                break;
            case Kind::kMark:
                what = "mark";
                script::run(pad.rdram, pad.ctx, pad.runtime,
                            "{set $" + action.var + " {player0 " + (action.streak ? "current_streak" : "score") + "}}");
                break;
            }
            if (action.kind != Kind::kRelease && action.kind != Kind::kMark)
                s_worst = std::max(s_worst, std::abs(now - action.at));
            std::cerr << "[play] " << what << " at " << now << " ms, " << now - action.at << " from " << action.at
                      << std::endl;
        }

        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const BeatMatcher matcher(rdram, GPR_U32(ctx, 4));
            const GuitarTrackWatcherImpl track(rdram, matcher.swingWatcher());
            if (!track.address || track.vtable() != s_addresses->guitarWatcherVtable || track.player() != 0)
                return;
            const float now = matcher.now() + track.sync();
            if (matcher.address != s_matcher)
            {
                s_matcher = matcher.address;
                s_held = 0u;
                s_frame = 0.0f;
                s_worst = 0.0f;
            }
            else if (now >= s_last)
                s_frame = std::min(now - s_last, 100.0f);
            s_last = now;
            if (s_queue.empty() || s_queue.front().at > now + s_frame / 2.0f)
                return;
            const EntryArgs args(ctx);
            const Pad pad{rdram, ctx, runtime};
            while (!s_queue.empty() && s_queue.front().at <= now + s_frame / 2.0f)
            {
                const Action action = s_queue.front();
                s_queue.erase(s_queue.begin());
                fire(pad, action, now);
            }
            args.restore(ctx);
        }

        struct DtorTag;
        void onDtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 4) != s_matcher)
                return;
            if (!s_queue.empty())
                std::cerr << "[play] " << s_queue.size() << " actions never went in" << std::endl;
            s_queue.clear();
            s_matcher = 0u;
        }

        // The first gem from `from` on that fits `kind`, or -1.
        int find(const GameGemList &gems, const std::string &kind, float from)
        {
            const int count = gems.size();
            const auto clear = [&](int i, int j, float ms) {
                return j < 0 || j >= count || std::abs(gems[j].ms() - gems[i].ms()) >= ms;
            };
            for (int i = 0; i < count; ++i)
            {
                if (gems[i].ms() < from)
                    continue;
                const bool one = std::popcount(gems[i].slots()) == 1;
                if (kind == "lone" && one && !gems[i].hopo() && clear(i, i - 1, 400.0f) && clear(i, i + 1, 400.0f))
                    return i;
                if (kind == "hammer" && one && i > 0 && gems[i].hopo() && std::popcount(gems[i - 1].slots()) == 1 &&
                    clear(i, i - 1, 150.0f) && clear(i, i + 1, 150.0f) && gems[i - 1].ms() >= from)
                    return i;
                if (kind == "run" && clear(i, i - 1, 170.0f) && i + 10 <= count)
                {
                    int k = i + 1;
                    while (k < i + 10 && clear(k, k - 1, 100.0f))
                        ++k;
                    if (k == i + 10)
                        return i;
                }
            }
            return -1;
        }

        script::Node command(const script::Call &call)
        {
            const auto integer = [](int value) { return script::Node{static_cast<uint32_t>(value), script::kInt}; };
            const std::string op = call.symbol(1);
            const GuitarTrackWatcherImpl track = watcher(call.rdram);
            const GameGemList gems = track.address ? track.gems() : GameGemList(call.rdram, 0u);
            const int count = track.address ? gems.size() : 0;
            const auto gemAt = [&](int i) { return i >= 0 && i < count; };
            if (op == "now")
                return script::floatNode(s_last);
            if (op == "gems")
                return integer(count);
            if (op == "hopos")
            {
                int hopos = 0;
                for (int i = 0; i < count; ++i)
                    hopos += gems[i].hopo() ? 1 : 0;
                return integer(hopos);
            }
            if (op == "idle")
                return integer(s_queue.empty() ? 1 : 0);
            if (op == "worst")
                return script::floatNode(s_worst);
            if (op == "dump")
            {
                for (int i = 0; i < count; ++i)
                    std::cerr << "[play] gem " << i << " " << gems[i].ms() << " ms, frets " << int(gems[i].slots())
                              << (gems[i].hopo() ? " hopo" : "") << std::endl;
                return integer(count);
            }
            if (op == "find" && track.address)
                return integer(find(gems, call.symbol(2), call.number(3)));
            if (op == "scoring")
            {
                const guest::DataArray config(call.rdram, static_cast<uint32_t>(call.runtime->callGuestFunction(
                                                              call.rdram, call.ctx, s_addresses->systemConfig, {})));
                const guest::DataArray scoring = config.find("scoring");
                const int difficulty = static_cast<int>(call.number(3));
                const guest::DataArray values =
                    call.symbol(2) == "crowd"
                        ? scoring.find("default_config").find("crowd").find(difficulty).find("range")
                        : scoring.find("star_ratings").find("thresholds").find(difficulty);
                const int at = call.symbol(2) == "crowd" ? 1 : 2;
                return script::floatNode(values.size() > at ? values.node(at).number() : -1.0f);
            }
            if (op == "release")
            {
                queue({call.number(2), Kind::kRelease});
                return integer(1);
            }
            if (op == "mark")
            {
                queue({call.number(2), Kind::kMark, 0u, call.symbol(3), call.symbol(4) == "streak"});
                return integer(1);
            }
            const int i = static_cast<int>(call.number(2));
            if (op == "gem" && gemAt(i))
            {
                const std::string field = call.symbol(3);
                if (field == "ms")
                    return script::floatNode(gems[i].ms());
                if (field == "frets")
                    return integer(std::popcount(gems[i].slots()));
                if (field == "hopo")
                    return integer(gems[i].hopo() ? 1 : 0);
                if (field == "played")
                    return integer(track.played(i) ? 1 : 0);
            }
            if ((op == "strum" || op == "press" || op == "hammer") && gemAt(i))
            {
                const Kind kind = op == "strum" ? Kind::kStrum : op == "press" ? Kind::kPress : Kind::kHammer;
                queue({gems[i].ms() + call.number(3), kind, gems[i].slots()});
                return integer(1);
            }
            if (op == "play" && gemAt(i))
            {
                const int last = std::min(count, i + static_cast<int>(call.number(3)));
                for (int g = i; g < last; ++g)
                    queue({gems[g].ms(), Kind::kStrum, gems[g].slots()});
                queue({gems[last - 1].ms() + 50.0f, Kind::kRelease});
                return integer(1);
            }
            std::cerr << "[play] usage: scripted_play.h; no " << op << (track.address ? "" : " with no guitar track")
                      << std::endl;
            return {};
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("ghc_play", command);
        EntryHook<PollTag>::install(runtime, addresses.beatMatcherPoll, onPoll);
        EntryHook<DtorTag>::install(runtime, addresses.beatMatcherDtor, onDtor);
    }
}
