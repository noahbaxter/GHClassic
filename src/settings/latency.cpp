// Audio and video latency, each the ms by which that half reaches the
// player late, from settings.ini.
//
// GH2 has one offset, Options +0x3C, negative when the player is late.
// Options::SetSyncOffset (0x10ded8) stores it and pushes it into the live
// BeatMatch (each TrackWatcherImpl, +0x28) and Players (+0x104); BeatMatch's
// constructor (0x120f48) builds its Players, then reads +0x3C for the new
// song. Only judgement reads it. The gems scroll on the song clock
// (TrackPanel::Poll reads TaskMgr::Seconds, which GamePanel::Poll sets from
// BeatMatch::Poll, which reads PlayerMatcher::GetSongMs).
//
// So the hit window follows the picture, offset = -video, and the song clock
// runs video - audio ahead of the audio it plays, so the sound and picture of
// one moment arrive together. The clock is StreamEE's VarTimer, held to the
// IOP's played position (synth.cpp), so the shift goes where the time is
// read, not into the timer.
//
// settings.ini owns both values, and every write of the game's offset is
// replaced, a save load's included. The game's lag screen, which measured
// audio but set the hit window, is replaced by src/ui/dta/lag_panel.dta, which
// sets the two through {latency ...}. The first save load seeds video from
// the save's offset, which leaves the hit window as it was; audio starts at
// its default, the port's own delay (settings.cpp).
//
// A sound the game times off that clock would come out video - audio late
// against the music. The one musical case: game.dta's beat handler claps in
// star power through a script task 0.9 beats on. So a script task on the
// beat clock that plays a sound starts that much sooner, back on the music's
// timeline; ones on song seconds (an explosion) stay with the picture.

#include "settings/latency.h"

#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "script.h"
#include "settings/settings.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
namespace gh2
{
    namespace
    {
        constexpr uint32_t kSyncOffset = 0x3cu; // Options::mSyncOffset
        constexpr uint32_t kTaskBeats = 1u;     // Task::Units
        constexpr uint32_t kScript = 0x4cu;     // ScriptTask's DataArray, from its ctor (0x2c5040)
        constexpr uint32_t kCommand = 0x11u, kProperty = 0x13u; // DataNode types holding an array

        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_getSongMs = nullptr;

        float syncOffset()
        {
            return -static_cast<float>(settings::get(settings::kVideoLagMs));
        }

        // Audio keeps its default: the save's offset is the hit window's.
        void seed(int lagMs)
        {
            settings::set(settings::kVideoLagMs, lagMs);
            settings::set(settings::kAudioLagMs, settings::get(settings::kAudioLagMs));
        }

        struct SetSyncOffsetTag;
        void onSetSyncOffset(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!settings::isSet(settings::kVideoLagMs))
                seed(static_cast<int>(std::lround(-ctx->f[12])));
            ctx->f[12] = syncOffset();
        }

        // A card with no save never calls SetSyncOffset.
        struct BeatMatchTag;
        void onBeatMatch(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            if (!settings::isSet(settings::kVideoLagMs))
                seed(0);
            store<float>(rdram, load<uint32_t>(rdram, s_addresses->theOptions) + kSyncOffset, syncOffset());
        }

        void getSongMs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            runtime->callGuestFunction(rdram, ctx, s_addresses->playerMatcherGetSongMs, {GPR_U32(ctx, 4)},
                                       s_getSongMs);
            ctx->f[0] += static_cast<float>(settings::get(settings::kVideoLagMs) -
                                            settings::get(settings::kAudioLagMs));
            ctx->pc = returnTo;
        }

        // DataArray: nodes at +0, size at +8; a node is {value, type}.
        bool playsSound(uint8_t *rdram, uint32_t array, int depth = 0)
        {
            if (array == 0u || depth > 8)
                return false;
            const uint32_t nodes = load<uint32_t>(rdram, array);
            const int16_t size = load<int16_t>(rdram, array + 8u);
            for (int16_t i = 0; i < size; ++i)
            {
                const uint32_t value = load<uint32_t>(rdram, nodes + 8u * static_cast<uint32_t>(i));
                const uint32_t type = load<uint32_t>(rdram, nodes + 8u * static_cast<uint32_t>(i) + 4u);
                if (type == script::kSymbol && value != 0u &&
                    std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, value)), "play_sfx") == 0)
                    return true;
                if ((type == script::kArray || type == kCommand || type == kProperty) && playsSound(rdram, value, depth + 1))
                    return true;
            }
            return false;
        }

        // TaskMgr::AddTask(task, units, delay), retail 0x2c6b18: the delay
        // counts on the clock at [+0x28] + units * 0x14, now at +0xC and the
        // last poll's at +0x10.
        struct AddTaskTag;
        void onAddTask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t task = GPR_U32(ctx, 5);
            if (GPR_U32(ctx, 6) != kTaskBeats || load<uint32_t>(rdram, task) != s_addresses->scriptTaskVtable ||
                !playsSound(rdram, load<uint32_t>(rdram, task + kScript)))
                return;
            const uint32_t clocks = load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u);
            const uint32_t seconds = clocks, beats = clocks + 0x14u * kTaskBeats;
            const float dBeat = load<float>(rdram, beats + 0xcu) - load<float>(rdram, beats + 0x10u);
            const float dSec = load<float>(rdram, seconds + 0xcu) - load<float>(rdram, seconds + 0x10u);
            if (dBeat <= 0.0f || dSec <= 0.0f)
                return;
            const float lateS = static_cast<float>(settings::get(settings::kAudioLagMs) -
                                                   settings::get(settings::kVideoLagMs)) / 1000.0f;
            ctx->f[12] = std::max(0.0f, ctx->f[12] - lateS * dBeat / dSec);
        }

        // {latency get video|audio}, {latency set video|audio <ms>}: the value
        // after, in ms. Setting video pushes the hit window to a song in play.
        //
        // {latency preview_at video|audio <impact> <period>}: for the lag
        // screen's preview, seconds into each beat of <period> to start the hit
        // animation (video) or play the click (audio), each early by its own
        // lateness, so both reach the player on the beat, <impact> in.
        script::Node latencyCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            const std::string half = call.symbol(2);
            if ((op != "get" && op != "set" && op != "preview_at") || (half != "video" && half != "audio"))
            {
                std::cerr << "[latency] usage: {latency get|set video|audio [ms]}, "
                             "{latency preview_at video|audio <impact> <period>}"
                          << std::endl;
                return {};
            }
            const settings::Key key = half == "video" ? settings::kVideoLagMs : settings::kAudioLagMs;
            if (op == "preview_at")
            {
                // The animation takes <impact> to land, so it starts that much
                // before the beat; the click plays on it.
                const float lateS = static_cast<float>(settings::get(key)) / 1000.0f;
                const float period = call.number(4);
                const float at = (half == "video" ? 0.0f : call.number(3)) - lateS;
                return script::floatNode(period > 0.0f ? at - period * std::floor(at / period) : 0.0f);
            }
            if (op == "set")
            {
                const script::Node ms = call.arg(3);
                if (ms.type != script::kInt)
                {
                    std::cerr << "[latency] set takes an int" << std::endl;
                    return {};
                }
                settings::set(key, static_cast<int32_t>(ms.value));
                const uint32_t options = load<uint32_t>(call.rdram, s_addresses->theOptions);
                if (key == settings::kVideoLagMs && options != 0u)
                    call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->optionsSetSyncOffset, {options});
            }
            return {static_cast<uint32_t>(settings::get(key)), script::kInt};
        }
    }

    void installLatency(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("latency", latencyCommand);
        script::runWhenUiReady(kLagPanelDta);
        EntryHook<SetSyncOffsetTag>::install(runtime, addresses.optionsSetSyncOffset, onSetSyncOffset);
        EntryHook<BeatMatchTag>::install(runtime, addresses.beatMatchCtor, onBeatMatch);
        s_getSongMs = runtime.lookupFunction(addresses.playerMatcherGetSongMs);
        runtime.replaceFunction(addresses.playerMatcherGetSongMs, &getSongMs);
        EntryHook<AddTaskTag>::install(runtime, addresses.taskMgrAddTask, onAddTask);
    }
}
