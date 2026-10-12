// Note speed: how fast gems come down the track, as Clone Hero counts it,
// one setting for easy and medium and one for hard and expert, as retail has
// a speed for each pair.
//
// A gem sits at ms * speed * 0.06 along the track (TrackConfig::GemSpacing,
// retail 0x152490, its speed at +0x18) and is first drawn horizon_y, 110,
// ahead of the now bar (track_graphics.dta; GemManager::Poll, 0x1564f8), so
// it is on the track for 110 / (60 * speed) seconds. Clone Hero's note speed
// n shows a note for 7.87 / n seconds, which puts hard and expert's 1.4 at
// 6.01. Taken as 6: speed n * 1.4 / 6.
//
// The settings are in tenths. The defaults are retail's: 60, and 43, which
// is easy and medium's speed of 1 exactly and not the tenth it is written
// as. The hyperspeed cheat, 1.5 times either (Track::Track, 0x14ff9c), is
// gone (game_settings.dta): 9 is what it made of hard and expert.
//
//   {note_speed stops}                  how many values a slider has
//   {note_speed stop easy|hard}         the one that setting is on, or nearest
//   {note_speed set_stop easy|hard <i>} sets it
//   {note_speed reset easy|hard}        to retail's; its stop
//   {note_speed value <i>}              stop i's setting

#include "note_speed.h"

#include "dta.h"
#include "hook.h"
#include "script.h"
#include "settings/settings.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace gh2
{
    namespace
    {
        struct TrackTag;

        constexpr int kEasyOwn = 43; // 30 / 7
        constexpr int kHardOwn = 60;
        constexpr float kEasySpeed = 1.0f, kHardSpeed = 1.4f; // track_speed

        float speed(int tenths)
        {
            if (tenths == kEasyOwn)
                return kEasySpeed;
            return static_cast<float>(tenths) * 0.1f * kHardSpeed / 6.0f;
        }

        // Easy and medium's own, then 5 to 20 by halves.
        const std::vector<int> &stops()
        {
            static const std::vector<int> s = []
            {
                std::vector<int> made = {kEasyOwn};
                for (int tenths = 50; tenths <= 200; tenths += 5)
                    made.push_back(tenths);
                return made;
            }();
            return s;
        }

        int stopOf(int tenths)
        {
            int nearest = 0;
            for (size_t i = 0; i < stops().size(); ++i)
                if (std::abs(stops()[i] - tenths) < std::abs(stops()[nearest] - tenths))
                    nearest = static_cast<int>(i);
            return nearest;
        }

        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            const int count = static_cast<int>(stops().size());
            const auto number = [](int value) { return script::Node{static_cast<uint32_t>(value), script::kInt}; };
            if (op == "stops")
                return number(count);
            if (op == "value")
            {
                const int i = static_cast<int>(call.number(2));
                return i >= 0 && i < count ? number(stops()[i]) : script::Node{};
            }
            const std::string which = call.symbol(2);
            if ((op != "stop" && op != "set_stop" && op != "reset") || (which != "easy" && which != "hard"))
            {
                std::cerr << "[note_speed] usage: {note_speed stops}, {note_speed stop|reset easy|hard}, "
                             "{note_speed set_stop easy|hard <i>}, {note_speed value <i>}"
                          << std::endl;
                return {};
            }
            const settings::Key key = which == "easy" ? settings::kNoteSpeedEasy : settings::kNoteSpeedHard;
            if (op == "reset")
                settings::set(key, which == "easy" ? kEasyOwn : kHardOwn);
            if (op == "set_stop")
                settings::set(key, stops()[std::clamp(static_cast<int>(call.number(3)), 0, count - 1)]);
            return number(stopOf(settings::get(key)));
        }

        // Track::Track(int, Symbol, RndBitmap *, float speed), retail
        // 0x14fa70: TrackPanel::CreateTracks (0x153a10) passes the mean of
        // the players' difficulties' track_speed, so a speed between the two
        // pairs' is two players on a difficulty of each, and takes as much of
        // each setting.
        void onTrack(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const float hard = std::clamp((ctx->f[12] - kEasySpeed) / (kHardSpeed - kEasySpeed), 0.0f, 1.0f);
            ctx->f[12] = speed(settings::get(settings::kNoteSpeedEasy)) * (1.0f - hard) +
                         speed(settings::get(settings::kNoteSpeedHard)) * hard;
        }
    }

    void installNoteSpeed(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<TrackTag>::install(runtime, addresses.trackCtor, onTrack);
        script::addCommand("note_speed", command);
        script::runWhenUiReady(kGameSettingsDta);
    }
}
