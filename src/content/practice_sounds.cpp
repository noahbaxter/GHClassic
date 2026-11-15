#include "content/practice_sounds.h"

#include "disc/ark.h"
#include "formats/midi.h"
#include "guest/beatmatch.h"
#include "hook.h"
#include "milo/milo.h"
#include "practice_sounds_bin.h"
#include "script.h"
#include "settings/settings.h"
#include "synth/adpcm.h"
#include "synth/synth.h"
#include "synth/timeline.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace gh2::practice_sounds
{
    namespace
    {
        constexpr uint32_t kRate = 22050u; // practice_sounds.py's
        // Scheduled this far ahead of the song clock: more than the mix
        // runs ahead of it, so each is in the timeline before it is due.
        constexpr double kAheadMs = 500.0;

        const Addresses *s_addresses = nullptr;
        // What each sound is to the timeline: practice_sounds.bin's by their
        // names, one part each; GH2's drum clicks by their Sfx's, a part a
        // sample it plays (a snare's left and right), each at its own
        // volume and pan.
        struct Part
        {
            int sound;
            float gain, pan;
        };
        std::map<std::string, std::vector<Part>> s_sounds;

        // The practice going on: its song's beats, each one's ms and
        // whether it starts a bar, and how far the timeline has been given.
        bool s_on = false;
        struct Beat
        {
            double ms;
            bool bar;
        };
        std::vector<Beat> s_beats;
        // Its drum clicks (TRIGGERS' 24 to 26), each one's ms and Sfx, and
        // whether they play (the part alone, ghc_practice_solo).
        struct Hit
        {
            double ms;
            const char *sfx;
        };
        std::vector<Hit> s_drums;
        bool s_drumsOn = false;
        double s_givenTo = -1.0; // song ms; below 0 nothing yet
        double s_lastNow = 0.0;

        // A SynthSample's sound to the timeline: its body is a revision, two
        // words, a byte, its file, a loop flag and two points, then 11, 2 for
        // PS-ADPCM, frames, rate, bytes, a byte and the data.
        std::optional<int> addSample(const milo::Bytes &b)
        {
            size_t o = 13u;
            milo::str(b, o);
            if (o + 30u > b.size() || milo::u32(b, o + 9u) != 11u || milo::u32(b, o + 13u) != 2u)
                return std::nullopt;
            const uint32_t rate = milo::u32(b, o + 21u), bytes = milo::u32(b, o + 25u);
            const size_t from = o + 30u;
            std::vector<int16_t> pcm;
            synth::AdpcmState state;
            for (size_t at = from; at + synth::kAdpcmBlockBytes <= std::min<size_t>(b.size(), from + bytes);
                 at += synth::kAdpcmBlockBytes)
            {
                int16_t block[synth::kAdpcmBlockSamples];
                synth::decodeAdpcm(b.data() + at, state, block);
                pcm.insert(pcm.end(), block, block + synth::kAdpcmBlockSamples);
                if (b[at + 1u] & synth::kAdpcmLoopEnd)
                    break;
            }
            return synth::timeline().addSound(pcm, rate);
        }

        float f32(const milo::Bytes &b, size_t o)
        {
            const uint32_t bits = milo::u32(b, o);
            float v;
            std::memcpy(&v, &bits, 4u);
            return v;
        }

        // An Sfx's parts: each SynthSample it names, followed in it by that
        // part's volume (dB) and pan.
        std::vector<Part> sfxParts(const milo::Dir &bank, const std::string &sfx)
        {
            std::vector<Part> out;
            const milo::Bytes *sound = milo::body(bank, "Sfx", sfx);
            if (!sound)
                return out;
            for (size_t i = 0; i < bank.entries.size(); ++i)
            {
                const auto &[cls, name] = bank.entries[i];
                if (cls != "SynthSample")
                    continue;
                milo::Bytes named;
                milo::putStr(named, name);
                const auto at = std::search(sound->begin(), sound->end(), named.begin(), named.end());
                const size_t o = static_cast<size_t>(at - sound->begin()) + named.size();
                if (at == sound->end() || o + 8u > sound->size())
                    continue;
                if (const auto id = addSample(bank.bodies[i]))
                    out.push_back({*id, std::pow(10.0f, f32(*sound, o) / 20.0f), f32(*sound, o + 4u)});
            }
            return out;
        }

        // GH2's drum clicks, from its practice bank.
        constexpr const char *kDrumSfx[] = {"practice_kick", "practice_snare", "practice_hat"};
        void addDrums()
        {
            const auto file = ark::readFile("sfx/gen/practice_bank.milo_ps2");
            const auto bank = file ? milo::read(*file) : std::nullopt;
            for (const char *sfx : kDrumSfx)
            {
                s_sounds[sfx] = bank ? sfxParts(*bank, sfx) : std::vector<Part>{};
                if (s_sounds[sfx].empty())
                    std::cerr << "[practice_sounds] no " << sfx << " in the practice bank" << std::endl;
            }
        }

        // practice_sounds.bin: a count, then each one's name (a byte of its
        // length first), frame count and 16-bit frames.
        void addSounds()
        {
            const auto u32 = [](size_t o)
            {
                uint32_t v;
                std::memcpy(&v, kPracticeSounds + o, 4u);
                return v;
            };
            size_t o = 4u;
            for (uint32_t i = 0; i < u32(0u) && o < sizeof kPracticeSounds; ++i)
            {
                const size_t length = kPracticeSounds[o++];
                std::string name(reinterpret_cast<const char *>(kPracticeSounds + o), length);
                const uint32_t frames = u32(o += length);
                std::vector<int16_t> pcm(frames);
                std::memcpy(pcm.data(), kPracticeSounds + o + 4u, frames * 2u);
                s_sounds[name] = {{synth::timeline().addSound(pcm, kRate), 1.0f, 0.0f}};
                o += 4u + frames * 2u;
            }
        }

        // A song's beats and drum clicks from its MIDI (a custom song's as
        // made for GH2, custom/gh2_chart.h): its tempos, and its time
        // signatures' beat (a quarter in 4/4, an eighth in 6/8), each bar's
        // first marked, to its last event; TRIGGERS' 24, 25 and 26, the kick,
        // snare and hi-hat (GH2's click_parser).
        void readSong(const std::string &song)
        {
            s_beats.clear();
            s_drums.clear();
            const auto bytes = ark::readFile("songs/" + song + "/" + song + ".mid");
            const auto file = bytes ? midi::parse(*bytes) : std::nullopt;
            if (!file || file->tracks.empty() || file->division == 0u)
                return;
            const midi::TempoMap tempos(*file);
            for (const midi::Track &t : file->tracks)
                if (t.name == "TRIGGERS")
                    for (const midi::Event &e : t.events)
                        if (midi::kind(e) == midi::kNoteOn && e.data.size() >= 2u && e.data[1] != 0u &&
                            e.data[0] >= 24u && e.data[0] <= 26u)
                            s_drums.push_back({tempos.seconds(e.tick) * 1000.0, kDrumSfx[e.data[0] - 24u]});
            uint32_t end = 0u;
            for (const midi::Track &t : file->tracks)
                if (!t.events.empty())
                    end = std::max(end, t.events.back().tick);
            // (tick, beats a bar, ticks a beat), from each signature on.
            std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> signatures = {{0u, 4u, file->division}};
            for (const midi::Event &e : file->tracks[0].events)
                if (e.status == midi::kMeta && e.meta == midi::kSignature && e.data.size() >= 2u && e.data[0] != 0u)
                {
                    const uint32_t per = file->division * 4u >> e.data[1];
                    if (per == 0u)
                        continue;
                    if (std::get<0>(signatures.back()) == e.tick)
                        signatures.pop_back();
                    signatures.push_back({e.tick, e.data[0], per});
                }
            for (size_t i = 0; i < signatures.size(); ++i)
            {
                const auto [from, beats, per] = signatures[i];
                const uint32_t to = i + 1u < signatures.size() ? std::get<0>(signatures[i + 1u]) : end + 1u;
                uint32_t n = 0u;
                for (uint32_t tick = from; tick < to; tick += per, ++n)
                    s_beats.push_back({tempos.seconds(tick) * 1000.0, n % beats == 0u});
            }
        }

        // A sound's every part at the song's `ms`.
        void play(const std::string &sound, double ms, float gain)
        {
            for (const Part &p : s_sounds[sound])
                synth::timeline().schedule(ms, p.sound, gain * p.gain, p.pan);
        }

        // The timeline given each of `times` in (from, to], by its ms.
        template <typename T, typename Sound>
        void give(const std::vector<T> &times, double from, double to, float gain, Sound sound)
        {
            auto it = std::upper_bound(times.begin(), times.end(), from, [](double ms, const T &t) { return ms < t.ms; });
            for (; it != times.end() && it->ms <= to; ++it)
                play(sound(*it), it->ms, gain);
        }

        // Practice loads the whole song and picks its sections as it plays
        // (game_settings.dta), but Player keeps the bounds it scores gems
        // within from when it was made: they are read again for each gem.
        constexpr uint32_t kSectionProvider = 0x208u; // GameConfig's PracticeSectionProvider
        constexpr uint32_t kPlayerBounds = 0xfcu;     // Player's first tick and the one past its last
        PS2Runtime::RecompiledFunction s_inBounds = nullptr;
        void inBounds(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_on)
            {
                const R5900Context saved = *ctx;
                const uint32_t player = GPR_U32(ctx, 4);
                runtime->callGuestFunction(rdram, ctx, s_addresses->gameConfigGetPracticeBounds,
                                           {load<uint32_t>(rdram, s_addresses->theGameConfig), player + kPlayerBounds,
                                            player + kPlayerBounds + 4u});
                *ctx = saved;
            }
            s_inBounds(rdram, ctx, runtime);
        }
        // SongDB::SetupPracticeSections, then in practice GameConfig's
        // sections read from it, which GH2 does only on its section screen:
        // what the song's Player and track are made with, and the pause
        // menu's list, come from them. GameConfig's mode Symbol is its +0x3c.
        PS2Runtime::RecompiledFunction s_setupSections = nullptr;
        void setupSections(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31), songDb = GPR_U32(ctx, 4);
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            callOriginal(rdram, ctx, runtime, s_addresses->songDbSetupPracticeSections, s_setupSections);
            const uint32_t mode = load<uint32_t>(rdram, config + 0x3cu);
            if (mode != 0u && std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), "practice") == 0)
                runtime->callGuestFunction(rdram, ctx, s_addresses->practiceSectionsInitFromDb,
                                           {config + kSectionProvider, songDb});
            ctx->pc = returnTo;
        }
        // PracticeSectionProvider::InitData, as Change Section's list takes it
        // (game_settings.dta): it loads the song into a SongDB and
        // MidiParserMgr of its own, the global one left 0 after, which the
        // song in play holds. In practice its sections are read already.
        PS2Runtime::RecompiledFunction s_initData = nullptr;
        void initData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_on)
                ctx->pc = GPR_U32(ctx, 31);
            else
                s_initData(rdram, ctx, runtime);
        }

        // TrackWatcherImpl::CheckForSustainedNoteTimeout(float ms), each
        // poll: what is due in the next kAheadMs given to the timeline,
        // player 1's notes (a chord or open one gem) and the beats.
        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const guest::TrackWatcherImpl watcher(rdram, GPR_U32(ctx, 4));
            if (!s_on || watcher.track() != 0u)
                return;
            const uint32_t clocks = load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u);
            const double now = load<float>(rdram, clocks + 0xcu) * 1000.0;
            // Back (a restart, the section round again): what was given
            // past here goes.
            if (s_givenTo < 0.0 || now < s_lastNow - 50.0)
            {
                synth::timeline().clear();
                s_givenTo = now;
            }
            s_lastNow = now;
            const double to = now + kAheadMs;
            if (to <= s_givenTo)
                return;
            const float gain = settings::get(settings::kFxVolume) / 11.0f;
            if (settings::get(settings::kPracticeNoteClick))
            {
                const guest::GameGemList gems = watcher.gems();
                for (int i = gems.closest(static_cast<float>(s_givenTo)); i < gems.size(); ++i)
                {
                    const double ms = gems[i].ms();
                    if (ms > to)
                        break;
                    if (ms > s_givenTo)
                        play("note_click", ms, gain);
                }
            }
            if (settings::get(settings::kPracticeMetronome))
                give(s_beats, s_givenTo, to, gain, [](const Beat &b) { return b.bar ? "metro_bar" : "metro_beat"; });
            if (s_drumsOn)
                give(s_drums, s_givenTo, to, gain, [](const Hit &h) { return h.sfx; });
            s_givenTo = to;
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addSounds();
        // {practice_section_count}: the song's sections, GameConfig's
        // PracticeSectionProvider's (+0x208), 0x18 bytes each from its +0x2c
        // to its +0x30.
        script::addCommand("practice_section_count", [](const script::Call &call) -> script::Node
                           {
                               const uint32_t provider = load<uint32_t>(call.rdram, s_addresses->theGameConfig) + 0x208u;
                               return {(load<uint32_t>(call.rdram, provider + 0x30u) - load<uint32_t>(call.rdram, provider + 0x2cu)) / 0x18u,
                                       script::kInt};
                           });
        EntryHook<PollTag>::install(runtime, addresses.trackWatcherSustainTimeout, onPoll);
        // {practice_clicks start <song>}: that song's practice begun;
        // {practice_clicks drums <bool>}: its drum clicks played or not;
        // {practice_clicks stop}: over; {practice_clicks played}: how many
        // the mix has started.
        script::addCommand("practice_clicks", [](const script::Call &call) -> script::Node
                           {
                               const std::string op = call.symbol(1);
                               if (op == "played")
                                   return {static_cast<uint32_t>(synth::timeline().started()), script::kInt};
                               if (op == "drums")
                               {
                                   s_drumsOn = call.number(2) != 0.0f;
                                   return {};
                               }
                               if (op == "start" && !s_sounds.count(kDrumSfx[0]))
                                   addDrums();
                               s_on = op == "start";
                               s_drumsOn = false;
                               if (s_on)
                                   readSong(call.symbol(2));
                               s_givenTo = -1.0;
                               synth::timeline().clear();
                               return {};
                           });
        script::addCommand("practice_option", [](const script::Call &call) -> script::Node
                           {
                               const std::string which = call.symbol(1);
                               const settings::Key key = which == "metronome"    ? settings::kPracticeMetronome
                                                         : which == "note_click" ? settings::kPracticeNoteClick
                                                                                 : settings::kPracticeSoloStems;
                               if (call.size() > 2)
                                   settings::set(key, call.number(2) != 0.0f ? 1 : 0);
                               return {settings::get(key) ? 1u : 0u, script::kInt};
                           });
        replaceKeeping(runtime, addresses.playerIsInBounds, &inBounds, s_inBounds);
        replaceKeeping(runtime, addresses.songDbSetupPracticeSections, &setupSections, s_setupSections);
        replaceKeeping(runtime, addresses.practiceSectionsInitData, &initData, s_initData);
    }
}
