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
//   {note_speed tenths easy|hard}       the setting itself, which the
//                                       settings file can put past the stops

#include "gameplay/note_speed.h"

#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "script.h"
#include "settings/settings.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        struct TrackTag;

        const Addresses *s_addresses = nullptr;

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
            if ((op != "stop" && op != "set_stop" && op != "reset" && op != "tenths") || (which != "easy" && which != "hard"))
            {
                std::cerr << "[note_speed] usage: {note_speed stops}, {note_speed stop|reset|tenths easy|hard}, "
                             "{note_speed set_stop easy|hard <i>}, {note_speed value <i>}"
                          << std::endl;
                return {};
            }
            const settings::Key key = which == "easy" ? settings::kNoteSpeedEasy : settings::kNoteSpeedHard;
            if (op == "tenths")
                return number(settings::get(key));
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

    namespace
    {
        struct FadeTag;
        struct DrawTag;

        constexpr int kOwnLength = 110; // horizon_y

        // How much longer than the game's own each Track was built, which it
        // stays until the song restarts whatever the setting does, and
        // where its camera is along it: at rest, as it is built; parked,
        // the furthest up it has been; and where it was last found and put.
        struct Built
        {
            float longer, camRest, camParked, camOwn, camPut;
            bool drawn = false;
        };
        std::unordered_map<uint32_t, Built> s_built;
        std::unordered_map<uint32_t, int32_t> s_ownRows; // a fade texture's own height

        // {highway_length} is the setting, {highway_length set <n>} sets it,
        // and game_settings.dta gives the track's config its horizon by it.
        // The board is not config: TrackSurface (retail 0x154bd0) copies its
        // meshes out of the track's scene, as long as they were drawn, a
        // vector of them at +0 and the mask over them at +0x10, and Track
        // has its side rails' at +0x84. Track::Track parents each under the
        // track's group after Track::SetupFade (0x150908), so here each is
        // made as much longer along the track as the horizon is further: its
        // RndTransformable is 0x40 into it, and its local's second row is
        // the way the track runs.
        //
        // The board's art (its Mat, the surface's +0x1c) scrolls under the
        // gems by its tex_xfm's position, and would be drawn out with the
        // mesh and so slide by them. Its v runs along the track, so tex_xfm
        // takes as much more of it and the art tiles, the size and at the
        // speed it has at the game's own length: about the origin, where the
        // Mat's own tex gen turns about the texture's centre and would scale
        // the scroll too. The two are the same with nothing to turn.
        void onSetupFade(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t track = GPR_U32(ctx, 4);
            const float by = static_cast<float>(settings::get(settings::kHighwayLength)) / kOwnLength;
            const float rest = load<float>(rdram, load<uint32_t>(rdram, track + 0xa4u) + milo::transformable::kLocal + 0x34u);
            s_built[track] = {by, rest, rest, rest, rest};
            const uint32_t surface = load<uint32_t>(rdram, track + 0x58u);
            // The scene's own Mat, which outlasts the track.
            const uint32_t mat = load<uint32_t>(rdram, surface + 0x1cu);
            const uint32_t texGen = mat != 0u ? load<uint32_t>(rdram, mat + milo::mat::kTexGen) : 0u;
            if (texGen == milo::mat::kTexGenXfm || texGen == milo::mat::kTexGenXfmOrigin)
            {
                store<uint32_t>(rdram, mat + milo::mat::kTexGen,
                                by == 1.0f ? milo::mat::kTexGenXfm : milo::mat::kTexGenXfmOrigin);
                store<float>(rdram, mat + milo::mat::kTexXfm + 0x14u, by);
            }
            if (by == 1.0f)
                return;
            std::vector<uint32_t> meshes = {load<uint32_t>(rdram, surface + 0x10u), load<uint32_t>(rdram, track + 0x84u)};
            for (uint32_t at = load<uint32_t>(rdram, surface); at < load<uint32_t>(rdram, surface + 4u); at += 4u)
                meshes.push_back(load<uint32_t>(rdram, at));
            for (const uint32_t mesh : meshes)
                for (uint32_t i = 0; mesh != 0u && i < 3u; ++i)
                {
                    const uint32_t along = mesh + milo::mesh::kTransform + milo::transformable::kLocal + 0x10u + 4u * i;
                    store<float>(rdram, along, load<float>(rdram, along) * by);
                }
        }

        // The far end fades into the venue behind it: Track::Draw (0x151188)
        // copies the picture there into track_fade.tex (the Track's +0x100)
        // before the track is drawn and draws it back over after, clear at
        // its bottom, in a rect that texture's size whose top is where
        // TrackSurface::GetFadeCenter (0x154fb0) finds track_fade.trans (the
        // surface's +0x14, the board's child, at its far end) on the screen
        // through the track's camera (+0xa4). Its 32 rows are about
        // alpha_dist (+0xf8) of track at the game's own length, and of a
        // longer one, whose end is further off, most of what was added. So
        // the texture is given the rows alpha_dist of this track takes,
        // measured by putting track_fade.trans that far short of the end
        // and asking where it shows.
        void onDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t track = GPR_U32(ctx, 4);
            const uint32_t tex = load<uint32_t>(rdram, track + 0x100u);
            const auto found = s_built.find(track);
            if (tex == 0u || found == s_built.end())
                return;
            Built &built = found->second;
            const int32_t own = s_ownRows.try_emplace(tex, load<int32_t>(rdram, tex + milo::tex::kHeight)).first->second;
            const float by = built.longer;
            const EntryArgs args(ctx);
            // A track made in a song (game_settings.dta's retrack) is drawn
            // the frame it is made, where the game's own first is under a
            // song's intro: its camera has no view yet, so nothing is found
            // through it and the far end goes unfaded for that frame. Draw
            // selects the camera after it has looked; here it is selected
            // before, once (RndCam::Select, by its vtable at +0xb8).
            if (!built.drawn)
            {
                built.drawn = true;
                const uint32_t cam = load<uint32_t>(rdram, track + 0xa4u);
                const uint32_t vtable = load<uint32_t>(rdram, cam + 0xb8u);
                runtime->callGuestFunction(rdram, ctx, load<uint32_t>(rdram, vtable + 0x14u),
                                           {cam + static_cast<uint32_t>(load<int16_t>(rdram, vtable + 0x10u))});
                args.restore(ctx);
            }
            if (by == 1.0f)
            {
                store<int32_t>(rdram, tex + milo::tex::kHeight, own);
                return;
            }
            const uint32_t surface = load<uint32_t>(rdram, track + 0x58u);
            const uint32_t trans = load<uint32_t>(rdram, surface + 0x14u);
            const uint32_t along = trans + milo::transformable::kLocal + 0x34u;
            const float end = load<float>(rdram, along);

            // Before a song the track waits out of sight: its camera stands
            // far enough up it to be past the board's end, and
            // extend_track_normal.tnm (Track::SetExtendTrackFrame, 0x1516c8)
            // brings it back to where it rests. A longer board's end is
            // further up than that, so the camera is put as much further as
            // the end is at its furthest, and less by how near it is to
            // rest. The animation writes where it has the camera only as it
            // plays, so what is found here is its own unless it is what was
            // last put.
            const uint32_t camAlong = load<uint32_t>(rdram, track + 0xa4u) + milo::transformable::kLocal + 0x34u;
            const float at = load<float>(rdram, camAlong);
            if (at != built.camPut)
                built.camOwn = at;
            built.camParked = std::max(built.camParked, built.camOwn);
            if (built.camParked > built.camRest)
            {
                const float out = std::clamp((built.camOwn - built.camRest) / (built.camParked - built.camRest), 0.0f, 1.0f);
                built.camPut = built.camOwn + end * (by - 1.0f) * out;
                if (built.camPut != at)
                {
                    store<float>(rdram, camAlong, built.camPut);
                    runtime->callGuestFunction(rdram, ctx, s_addresses->setDirty, {load<uint32_t>(rdram, track + 0xa4u)});
                }
            }
            // GetFadeCenter's two ints, on the guest stack.
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t out = sp - 0x10u;
            SET_GPR_U32(ctx, 29, out);
            const auto row = [&](float at)
            {
                store<float>(rdram, along, at);
                runtime->callGuestFunction(rdram, ctx, s_addresses->setDirty, {trans});
                runtime->callGuestFunction(rdram, ctx, s_addresses->trackSurfaceGetFadeCenter,
                                           {surface, out, out + 4u, load<uint32_t>(rdram, track + 0xa4u)});
                return load<int32_t>(rdram, out + 4u);
            };
            // In the board's own measure, which is the track's over how much
            // longer it is; then back at the end.
            const int32_t bottom = row(end - load<float>(rdram, track + 0xf8u) / by);
            const int32_t top = row(end);
            SET_GPR_U32(ctx, 29, sp);
            args.restore(ctx);
            // Within reason: as the track swings in at a song's start, some
            // of it is behind the camera.
            store<int32_t>(rdram, tex + milo::tex::kHeight, std::clamp(bottom - top, 1, 4 * own));
        }

        script::Node lengthCommand(const script::Call &call)
        {
            if (call.size() > 2 && call.symbol(1) == "set")
                settings::set(settings::kHighwayLength, static_cast<int>(call.number(2)));
            return {static_cast<uint32_t>(settings::get(settings::kHighwayLength)), script::kInt};
        }
    }

    void installNoteSpeed(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<TrackTag>::install(runtime, addresses.trackCtor, onTrack);
        s_addresses = &addresses;
        EntryHook<FadeTag>::install(runtime, addresses.trackSetupFade, onSetupFade);
        EntryHook<DrawTag>::install(runtime, addresses.trackDraw, onDraw);
        script::addCommand("note_speed", command);
        script::addCommand("highway_length", lengthCommand);
        // The pause menu's among them: a song's own.
        script::patchUi(kGameSettingsDta, true);
    }
}
