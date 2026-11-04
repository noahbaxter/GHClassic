// Custom songs' charts read on the host: a small .chart and .mid of each
// note type, checked; then, given folders, every song.ini's chart under
// them read and tallied. Exits 1 on any failed check.
//
//   custom_chart_test [folder ...]

#include "content/pcm_mix.h"
#include "custom/catalog.h"
#include "custom/chart.h"
#include "custom/gh2_chart.h"
#include "custom/index.h"
#include "custom/library.h"
#include "custom/song_ini.h"
#include "formats/md5.h"
#include "formats/midi.h"
#include "formats/sng.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using namespace gh2;
    using namespace gh2::custom;
    namespace fs = std::filesystem;
    namespace midi = gh2::midi;

    int s_failed = 0;

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
            ++s_failed; \
        } \
    } while (0)

    // The least a song folder needs: a one-gem chart, and the head of an Ogg
    // page that names Vorbis for its stem.
    const std::string kOneGem = "[Song]\n{\n  Resolution = 192\n}\n[ExpertSingle]\n{\n  0 = N 0 0\n}\n";
    const std::string kVorbisHead = "OggS" + std::string(24, '\0') + "\x01vorbis";

    const Gem *at(const Difficulty &d, uint32_t tick)
    {
        for (const Gem &g : d.gems)
            if (g.tick == tick)
                return &g;
        return nullptr;
    }

    // A stereo stem holding one level throughout.
    class Level final : public vgs::Pcm
    {
    public:
        Level(uint32_t at, uint64_t length, float level) : m_level(level)
        {
            channels = 2u;
            rate = at;
            frames = length;
        }

        bool seek(uint64_t frame) override
        {
            m_at = frame;
            return true;
        }

        uint32_t read(float *const *out, uint32_t most) override
        {
            const auto n = static_cast<uint32_t>(std::min<uint64_t>(most, frames - std::min(m_at, frames)));
            for (uint32_t c = 0; c < channels; ++c)
                std::fill(out[c], out[c] + n, m_level);
            m_at += n;
            return n;
        }

    private:
        float m_level;
        uint64_t m_at = 0u;
    };

    // A stem at 48 kHz (as Opus always is) beside one at 44.1 (as MP3 and
    // Vorbis mostly are), a second of each: both heard through the whole
    // mix, the 44.1 one read to 48.
    void mixedRates()
    {
        vgs::Mixer mix;
        mix.inputs.push_back({std::make_unique<Level>(48000u, 48000u, 0.25f), 0u});
        mix.inputs.push_back(
            {std::make_unique<vgs::Resampled>(std::make_unique<Level>(44100u, 44100u, 0.5f), 48000u), 1u});
        mix.channels = 4u;
        mix.rate = 48000u;
        mix.frames = 48000u;
        CHECK(mix.inputs[1].pcm->frames >= 47999u);
        std::vector<std::vector<float>> out(4u, std::vector<float>(48000u));
        std::vector<float *> to;
        for (auto &channel : out)
            to.push_back(channel.data());
        mix.seek(0u);
        CHECK(mix.fill(to.data(), 48000u) == 48000u);
        for (const uint32_t frame : {0u, 24000u, 47990u})
        {
            CHECK(out[0][frame] == 0.25f && out[1][frame] == 0.25f);
            CHECK(out[2][frame] == 0.5f && out[3][frame] == 0.5f);
        }
    }

    void chartTypes()
    {
        const std::string text = "[Song]\n{\n  Resolution = 192\n  Offset = 0.5\n}\n"
                                 "[SyncTrack]\n{\n  0 = TS 4\n  0 = B 120000\n  768 = B 240000\n}\n"
                                 "[Events]\n{\n  0 = E \"section Intro\"\n}\n"
                                 "[ExpertSingle]\n{\n"
                                 "  0 = N 0 0\n"             // green, strum (first)
                                 "  48 = N 1 0\n"            // red, natural HOPO
                                 "  96 = N 1 0\n"            // red again, strum
                                 "  144 = N 2 0\n  144 = N 5 0\n" // yellow, natural HOPO flipped to strum
                                 "  384 = N 3 0\n  384 = N 5 0\n" // blue, far: strum flipped to HOPO
                                 "  432 = N 4 0\n  432 = N 6 0\n" // orange tap
                                 "  480 = N 7 96\n"          // open, sustained
                                 "  768 = N 0 384\n  768 = N 2 96\n" // chord, lanes ending apart
                                 "  768 = S 2 384\n"
                                 "  900 = E solo\n  1000 = E soloend\n"
                                 "}\n";
        std::string why;
        const auto chart = parseChart(text, {}, why);
        CHECK(chart);
        if (!chart)
            return;
        CHECK(chart->resolution == 192u);
        CHECK(chart->offset == 0.5);
        CHECK(chart->tempos.size() == 2u && chart->tempos[1].usPerBeat == 250000u);
        CHECK(chart->seconds(768u) == 2.0);
        CHECK(chart->seconds(960u) == 2.25);
        CHECK(chart->sections.size() == 1u && chart->sections[0].second == "Intro");
        CHECK(chart->parts.count(Part::Guitar));
        const Difficulty &x = chart->parts.at(Part::Guitar)[3];
        CHECK(x.gems.size() == 8u);
        CHECK(at(x, 0) && at(x, 0)->kind == Kind::Strum && !at(x, 0)->forced);
        CHECK(at(x, 48) && at(x, 48)->kind == Kind::Hopo && !at(x, 48)->forced);
        CHECK(at(x, 96) && at(x, 96)->kind == Kind::Strum);
        CHECK(at(x, 144) && at(x, 144)->kind == Kind::Strum && at(x, 144)->flip && !at(x, 144)->forced);
        CHECK(at(x, 384) && at(x, 384)->kind == Kind::Hopo && at(x, 384)->flip && !at(x, 384)->forced);
        CHECK(at(x, 432) && at(x, 432)->kind == Kind::Tap);
        CHECK(at(x, 480) && at(x, 480)->lanes == 1u << kOpen && at(x, 480)->length[kOpen] == 96u);
        const Gem *chord = at(x, 768);
        CHECK(chord && chord->chord() && chord->split() && chord->longest() == 384u);
        CHECK(x.starPower.size() == 1u && x.starPower[0].length == 384u);
        CHECK(x.solos.size() == 1u && x.solos[0].tick == 900u && x.solos[0].length == 100u);

        // A tempo listed out of order put in its place, no 120 put before it.
        const auto unsorted = parseChart("[SyncTrack]\n{\n  768 = B 240000\n  0 = B 60000\n}\n", {}, why);
        CHECK(unsorted && unsorted->tempos.size() == 2u && unsorted->tempos[0].usPerBeat == 1000000u);

        // GH2's rule in a .chart too: a note on a fret of the chord before
        // is a strum, one off it a HOPO.
        const auto outOfChord =
            parseChart("[ExpertSingle]\n{\n  0 = N 0 0\n  0 = N 1 0\n  48 = N 0 0\n  96 = N 0 0\n  96 = N 1 0\n"
                       "  144 = N 2 0\n}\n",
                       {}, why);
        CHECK(outOfChord && outOfChord->parts.count(Part::Guitar));
        if (outOfChord && outOfChord->parts.count(Part::Guitar))
        {
            const Difficulty &o = outOfChord->parts.at(Part::Guitar)[3];
            CHECK(at(o, 48) && at(o, 48)->kind == Kind::Strum);
            CHECK(at(o, 144) && at(o, 144)->kind == Kind::Hopo);
        }

        // GH2's threshold, under 170 at 480: 67 ticks at 192 is 168, a
        // HOPO; 71 is 177, a strum. Clone Hero's, 65 at 192, strums both.
        const std::string gaps = "[ExpertSingle]\n{\n  0 = N 0 0\n  67 = N 1 0\n  138 = N 2 0\n}\n";
        const auto natural = parseChart(gaps, {}, why);
        CHECK(natural && natural->parts.count(Part::Guitar) && !natural->hopoThreshold);
        if (natural && natural->parts.count(Part::Guitar))
        {
            const Difficulty &n = natural->parts.at(Part::Guitar)[3];
            CHECK(at(n, 67) && at(n, 67)->kind == Kind::Hopo);
            CHECK(at(n, 138) && at(n, 138)->kind == Kind::Strum);
        }
        // song.ini's own: 100 at 192 is 250 at 480; GH2's 170 (68 at 192)
        // and an eighth's 240 by eighthnote_hopo.
        Options wide;
        wide.hopoFrequency = 100u;
        const auto widened = parseChart(gaps, wide, why);
        CHECK(widened && widened->hopoThreshold == 250u);
        if (widened && widened->parts.count(Part::Guitar))
            CHECK(at(widened->parts.at(Part::Guitar)[3], 138)->kind == Kind::Hopo);
        Options stamped;
        stamped.hopoFrequency = 68u;
        const auto ripped = parseChart(gaps, stamped, why);
        CHECK(ripped && !ripped->hopoThreshold);
        Options eighth;
        eighth.eighthNoteHopo = true;
        const auto eighths = parseChart(gaps, eighth, why);
        CHECK(eighths && eighths->hopoThreshold == 240u);

        // Notes within 10 ticks at 480 (4 at 192) are one chord, as GH2's
        // parser makes them.
        const auto near = parseChart("[ExpertSingle]\n{\n  0 = N 0 0\n  4 = N 1 0\n  9 = N 2 0\n}\n", {}, why);
        CHECK(near && near->parts.count(Part::Guitar));
        if (near && near->parts.count(Part::Guitar))
        {
            const Difficulty &n = near->parts.at(Part::Guitar)[3];
            CHECK(n.gems.size() == 2u && at(n, 0) && at(n, 0)->lanes == 0x3u && at(n, 9));
        }

        // Each player's Face-Off phrases.
        const auto faceOff = parseChart("[ExpertSingle]\n{\n  0 = N 0 0\n  0 = S 0 192\n  384 = S 1 96\n}\n", {}, why);
        CHECK(faceOff && faceOff->parts.count(Part::Guitar));
        if (faceOff && faceOff->parts.count(Part::Guitar))
        {
            const Difficulty &f = faceOff->parts.at(Part::Guitar)[3];
            CHECK(f.faceOff[0].size() == 1u && f.faceOff[0][0].length == 192u);
            CHECK(f.faceOff[1].size() == 1u && f.faceOff[1][0].tick == 384u);
        }

        // As GH2 takes it: 480 a beat, the open as all five frets, star
        // power on 103.
        const auto gh2 = toGh2(*chart, 0.0);
        CHECK(gh2);
        if (!gh2)
            return;
        const auto back = gh2::midi::parse(gh2->mid);
        CHECK(back && back->division == 480u && back->tracks.size() == 3u);
        if (!back || back->tracks.size() != 3u)
            return;
        std::map<uint8_t, std::vector<uint32_t>> ons;
        for (const midi::Event &e : back->tracks[1].events)
            if (midi::kind(e) == midi::kNoteOn)
                ons[e.data[0]].push_back(e.tick);
        CHECK(ons[96] == (std::vector<uint32_t>{0u, 1200u, 1920u}));
        CHECK(ons[97] == (std::vector<uint32_t>{120u, 240u, 1200u}));
        CHECK(ons[103] == (std::vector<uint32_t>{1920u}));
        CHECK(ons.size() == 6u); // five frets and star power
        // Each gem's kind beside it, at 480 a beat: the tap hammers on, and
        // so does the open close after it; the chord is strummed.
        const auto &kinds = gh2->gems[3];
        const auto hopo = [&](uint32_t tick) { return kinds.count(tick) && kinds.at(tick).hopo; };
        CHECK(kinds.size() == 8u);
        CHECK(!hopo(0) && hopo(120) && !hopo(240) && !hopo(360) && hopo(960) && hopo(1080) && !hopo(1920));
        CHECK(kinds.count(1920) && kinds.at(1920).frets == 0x5u);
        // The chord's lanes apart, at 240 a minute from 768: green 500 ms,
        // yellow 125.
        CHECK(kinds.count(1920) && kinds.at(1920).split() && kinds.at(1920).laneMs[0] == 500.0f &&
              kinds.at(1920).laneMs[2] == 125.0f && kinds.at(1920).laneMs[1] == 0.0f);
        CHECK(kinds.count(0) && !kinds.at(0).split());
        CHECK(kinds.count(1200) && kinds.at(1200).open && kinds.at(1200).frets == kOpenFrets && hopo(1200));
        bool end = false, section = false;
        for (const midi::Event &e : back->tracks[2].events)
        {
            const std::string text(e.data.begin(), e.data.end());
            end |= e.status == midi::kMeta && text == "[end]";
            section |= e.status == midi::kMeta && text == "[section custom_section_0]";
        }
        CHECK(end && section);
    }

    // Sustains held past the next gem: green under red and yellow, kept
    // whole beside the MIDI's, cut where it starts the next; blue cut by the
    // blue after it, as long as the MIDI's, so none beside it.
    void heldPastNext()
    {
        const std::string text = "[Song]\n{\n  Resolution = 192\n}\n"
                                 "[SyncTrack]\n{\n  0 = TS 4\n  0 = B 120000\n}\n"
                                 "[ExpertSingle]\n{\n"
                                 "  0 = N 0 768\n  192 = N 1 0\n  384 = N 2 0\n"
                                 "  960 = N 3 768\n  1152 = N 3 0\n"
                                 "}\n";
        std::string why;
        const auto chart = parseChart(text, {}, why);
        const auto gh2 = chart ? toGh2(*chart, 0.0) : std::nullopt;
        CHECK(gh2);
        if (!gh2)
            return;
        const auto &gems = gh2->gems[3];
        CHECK(gems.count(0) && gems.at(0).sustain == 1920u && gems.at(0).sustainMs == 2000.0f);
        CHECK(gems.count(480) && gems.at(480).sustain == 0u);
        CHECK(gems.count(2400) && gems.at(2400).sustain == 0u);
        const auto back = midi::parse(gh2->mid);
        std::set<uint32_t> greenOff;
        if (back && back->tracks.size() > 1u)
            for (const midi::Event &e : back->tracks[1].events)
                if (midi::kind(e) == midi::kNoteOff && e.data[0] == 96u)
                    greenOff.insert(e.tick);
        CHECK(greenOff.count(480u));

        // A chord whose lanes end apart: the longest in the MIDI, each lane
        // its own beside it.
        const auto apart = parseChart("[Song]\n{\n  Resolution = 192\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                      "[ExpertSingle]\n{\n  0 = N 0 384\n  0 = N 1 96\n  960 = N 2 0\n}\n",
                                      {}, why);
        const auto offsOf = [](const std::optional<Gh2Chart> &c)
        {
            std::set<uint32_t> offs;
            const auto m = c ? midi::parse(c->mid) : std::nullopt;
            if (m && m->tracks.size() > 1u)
                for (const midi::Event &e : m->tracks[1].events)
                    if (midi::kind(e) == midi::kNoteOff && (e.data[0] == 96u || e.data[0] == 97u))
                        offs.insert(e.tick);
            return offs;
        };
        const auto longest = apart ? toGh2(*apart, 0.0) : std::nullopt;
        CHECK(longest && offsOf(longest) == std::set<uint32_t>{960u} && longest->gems[3].at(0).split());

        // A sustain held past the last gem: the song ends past it, not past
        // the last gem (else GH2 tears the track down under the held one).
        const auto past = parseChart("[Song]\n{\n  Resolution = 192\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                     "[ExpertSingle]\n{\n  0 = N 0 768\n  192 = N 1 0\n}\n",
                                     {}, why);
        const auto pastGh2 = past ? toGh2(*past, 0.0) : std::nullopt;
        const auto pastMid = pastGh2 ? midi::parse(pastGh2->mid) : std::nullopt;
        uint32_t endAt = 0u;
        if (pastMid && pastMid->tracks.size() > 2u)
            for (const midi::Event &e : pastMid->tracks[2].events)
                if (e.status == midi::kMeta && std::string(e.data.begin(), e.data.end()) == "[end]")
                    endAt = e.tick;
        CHECK(pastGh2 && pastGh2->gems[3].count(0) && pastGh2->gems[3].at(0).sustain == 1920u);
        CHECK(endAt >= 1920u + 960u);
    }

    using midi::note;

    void midTypes()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0x07u, 0xa1u, 0x20u}});
        midi::Track guitar;
        guitar.name = "PART GUITAR";
        guitar.events.push_back(midi::text(0u, "[ENHANCED_OPENS]"));
        const auto hold = [&](uint32_t tick, uint8_t pitch, uint32_t length)
        {
            guitar.events.push_back(note(tick, pitch, true));
            guitar.events.push_back(note(tick + length, pitch, false));
        };
        hold(0u, 96u, 60u);     // green, short: kept as written, the engine cuts it
        hold(120u, 97u, 60u);   // red, natural HOPO
        hold(240u, 98u, 60u);   // yellow, forced strum
        hold(240u, 102u, 60u);
        hold(960u, 99u, 60u);   // blue, forced HOPO
        hold(960u, 101u, 60u);
        hold(1200u, 100u, 60u); // orange tap
        hold(1200u, 104u, 60u);
        hold(1440u, 95u, 480u); // open
        hold(1920u, 96u, 960u); // a chord ending apart
        hold(1920u, 98u, 480u);
        hold(1920u, 116u, 960u);
        hold(1920u, 103u, 240u);
        // A Phase Shift open, all difficulties.
        guitar.events.push_back({2880u, midi::kSysex, 0u, {'P', 'S', 0u, 0u, 0xffu, 1u, 1u, midi::kSysexEnd}});
        hold(2880u, 96u, 60u);
        guitar.events.push_back({2940u, midi::kSysex, 0u, {'P', 'S', 0u, 0u, 0xffu, 1u, 0u, midi::kSysexEnd}});
        hold(3360u, 96u, 60u); // chord
        hold(3360u, 97u, 60u);
        hold(3480u, 96u, 60u); // green out of it: a strum, in a .mid
        std::stable_sort(guitar.events.begin(), guitar.events.end(),
                         [](const midi::Event &a, const midi::Event &b) { return a.tick < b.tick; });
        midi::Track events;
        events.name = "EVENTS";
        events.events.push_back(midi::text(0u, "[section verse_1]"));
        events.events.push_back(midi::text(960u, "[prc_chorus_1]"));
        file.tracks = {tempo, guitar, events};
        std::string why;
        const auto bytes = midi::write(file);
        const auto chart = parseMid(bytes, {}, why);
        CHECK(chart);
        if (!chart)
            return;
        // Told by its name, whatever its case.
        CHECK(parseSongChart("Notes.MID", std::string(bytes.begin(), bytes.end()), {}, why));
        CHECK(chart->resolution == 480u);
        CHECK(chart->tempos.size() == 1u && chart->tempos[0].usPerBeat == 500000u);
        CHECK(chart->sections.size() == 2u && chart->sections[1].second == "chorus_1");
        CHECK(chart->parts.count(Part::Guitar));
        const Difficulty &x = chart->parts.at(Part::Guitar)[3];
        CHECK(x.gems.size() == 10u);
        CHECK(at(x, 3480) && at(x, 3480)->kind == Kind::Strum);
        CHECK(at(x, 0) && at(x, 0)->longest() == 60u);
        CHECK(at(x, 120) && at(x, 120)->kind == Kind::Hopo && !at(x, 120)->forced);
        CHECK(at(x, 240) && at(x, 240)->kind == Kind::Strum && at(x, 240)->forced);
        CHECK(at(x, 960) && at(x, 960)->kind == Kind::Hopo && at(x, 960)->forced);
        CHECK(at(x, 1200) && at(x, 1200)->kind == Kind::Tap);
        CHECK(at(x, 1440) && at(x, 1440)->lanes == 1u << kOpen && at(x, 1440)->length[kOpen] == 480u);
        CHECK(at(x, 1920) && at(x, 1920)->split());
        CHECK(at(x, 2880) && at(x, 2880)->lanes == 1u << kOpen);
        CHECK(x.starPower.size() == 1u && x.solos.size() == 1u);
    }

    // A GH1 or GH2 rip's markers: each difficulty's star power and Face-Off
    // phrases, big notes; Rock Band's 105 and 106 on all four, its big rock
    // ending.
    void midMarkers()
    {
        const auto chartOf = [](const std::vector<std::pair<uint8_t, uint32_t>> &notes)
        {
            midi::File file{1u, 480u, {midi::Track{}}};
            midi::Track guitar;
            guitar.name = "PART GUITAR";
            for (const auto &[pitch, tick] : notes)
            {
                guitar.events.push_back(note(tick, pitch, true));
                guitar.events.push_back(note(tick + 240u, pitch, false));
            }
            std::stable_sort(guitar.events.begin(), guitar.events.end(),
                             [](const midi::Event &a, const midi::Event &b) { return a.tick < b.tick; });
            file.tracks.push_back(guitar);
            std::string why;
            return parseMid(midi::write(file), {}, why);
        };
        const auto gh = chartOf({{60u, 0u}, {96u, 0u}, {67u, 0u}, {103u, 960u}, {69u, 0u}, {106u, 480u},
                                 {110u, 1920u}});
        CHECK(gh && gh->parts.count(Part::Guitar));
        if (gh && gh->parts.count(Part::Guitar))
        {
            const auto &d = gh->parts.at(Part::Guitar);
            CHECK(d[0].starPower.size() == 1u && d[0].starPower[0].tick == 0u);
            CHECK(d[1].starPower.empty() && d[3].starPower.size() == 1u && d[3].starPower[0].tick == 960u);
            CHECK(d[3].solos.empty());
            CHECK(d[0].faceOff[0].size() == 1u && d[0].faceOff[1].empty());
            CHECK(d[3].faceOff[0].empty() && d[3].faceOff[1].size() == 1u);
            CHECK(d[3].bigNotes.size() == 1u && d[3].bigNotes[0].tick == 1920u);
            // Written for GH2's own parsers: 67 Easy's star power, 69 Easy's
            // first player, 106 Expert's second, 110.
            const auto gh2 = toGh2(*gh, 0.0);
            const auto back = gh2 ? gh2::midi::parse(gh2->mid) : std::nullopt;
            CHECK(back && back->tracks.size() == 3u);
            if (back && back->tracks.size() == 3u)
            {
                const auto has = [&](uint8_t pitch, uint32_t tick)
                {
                    return std::any_of(back->tracks[1].events.begin(), back->tracks[1].events.end(),
                                       [&](const midi::Event &e) {
                                           return midi::kind(e) == midi::kNoteOn && e.data.size() >= 2u &&
                                                  e.data[0] == pitch && e.data[1] != 0u && e.tick == tick;
                                       });
                };
                CHECK(has(67u, 0u) && has(103u, 960u) && !has(79u, 0u) && has(110u, 1920u));
                CHECK(has(69u, 0u) && has(106u, 480u) && !has(70u, 0u) && !has(105u, 0u));
            }
        }
        const auto rb = chartOf({{96u, 0u}, {116u, 0u}, {103u, 480u}, {105u, 0u}, {106u, 960u}, {120u, 1920u}});
        CHECK(rb && rb->parts.count(Part::Guitar));
        if (rb && rb->parts.count(Part::Guitar))
        {
            const auto &d = rb->parts.at(Part::Guitar);
            CHECK(d[0].starPower.size() == 1u && d[0].starPower[0].tick == 0u && d[3].solos.size() == 1u);
            CHECK(d[0].faceOff[0].size() == 1u && d[0].faceOff[1].size() == 1u && d[0].faceOff[1][0].tick == 960u);
            CHECK(d[3].rockEndings.size() == 1u && d[3].rockEndings[0].tick == 1920u);
            // Rock Band's 105 and 106 written onto every difficulty.
            const auto gh2 = toGh2(*rb, 0.0);
            const auto back = gh2 ? gh2::midi::parse(gh2->mid) : std::nullopt;
            CHECK(back && back->tracks.size() == 3u);
            if (back && back->tracks.size() == 3u)
                for (const uint8_t base : {60u, 72u, 84u, 96u})
                {
                    const auto count = std::count_if(back->tracks[1].events.begin(), back->tracks[1].events.end(),
                                                     [&](const midi::Event &e) {
                                                         return midi::kind(e) == midi::kNoteOn && e.data.size() >= 2u &&
                                                                e.data[1] != 0u &&
                                                                ((e.data[0] == base + 9u && e.tick == 0u) ||
                                                                 (e.data[0] == base + 10u && e.tick == 960u));
                                                     });
                    CHECK(count == 2);
                }
        }
    }

    // Trill and tremolo lanes, as YARG picks their notes: a tremolo's one
    // mask, cut where it changes; a trill's two single notes alternating;
    // one that never starts right, no lane. Sustains cut but the last's.
    // Hard's only where struck at velocity 41 to 50.
    void lanes()
    {
        std::string why;
        const auto chart = parseChart("[ExpertSingle]\n{\n"
                                      "  0 = S 65 300\n  0 = N 1 50\n  96 = N 1 50\n  192 = N 1 50\n  288 = N 2 0\n"
                                      "  768 = S 66 400\n  768 = N 0 0\n  864 = N 2 0\n  960 = N 0 0\n  1056 = N 2 0\n"
                                      "  1152 = N 3 300\n"
                                      "  1536 = S 66 300\n  1536 = N 0 0\n  1632 = N 2 0\n  1728 = N 3 0\n"
                                      "  2304 = S 65 200\n  2304 = N 7 0\n  2400 = N 7 0\n  2496 = N 7 0\n"
                                      "  3072 = S 66 300\n  3072 = N 7 0\n  3168 = N 0 0\n  3264 = N 7 0\n  3360 = N 0 0\n}\n",
                                      {}, why);
        CHECK(chart && chart->parts.count(Part::Guitar));
        if (!chart || !chart->parts.count(Part::Guitar))
            return;
        const Difficulty &x = chart->parts.at(Part::Guitar)[3];
        CHECK(at(x, 0) && at(x, 0)->laned && at(x, 0)->laneStart && at(x, 0)->longest() == 0u);
        CHECK(at(x, 192) && at(x, 192)->laned && !at(x, 192)->laneStart && at(x, 192)->longest() == 50u);
        CHECK(at(x, 288) && !at(x, 288)->laned);
        CHECK(at(x, 768) && at(x, 768)->laneStart && at(x, 1056) && at(x, 1056)->laned);
        CHECK(at(x, 1152) && !at(x, 1152)->laned && at(x, 1152)->longest() == 300u);
        CHECK(at(x, 1536) && !at(x, 1536)->laned);
        // Red's band, then green's and yellow's, then the opens', then a
        // trill of an open and green's, one band each.
        CHECK(x.bands.size() == 6u);
        if (x.bands.size() == 6u)
        {
            CHECK(x.bands[0].lane == 1 && x.bands[0].tick == 0u && x.bands[0].length == 192u);
            CHECK(x.bands[1].lane == 0 && x.bands[1].tick == 768u && x.bands[1].length == 192u);
            CHECK(x.bands[2].lane == 2 && x.bands[2].tick == 864u && x.bands[2].length == 192u);
            CHECK(x.bands[3].lane == kOpen && x.bands[3].tick == 2304u && x.bands[3].length == 192u);
            CHECK(x.bands[4].lane == kOpen && x.bands[4].tick == 3072u && x.bands[4].length == 192u);
            CHECK(x.bands[5].lane == 0 && x.bands[5].tick == 3168u && x.bands[5].length == 192u);
        }
        CHECK(at(x, 3072) && at(x, 3072)->laneStart && at(x, 3360) && at(x, 3360)->laned);

        const auto midOf = [&](uint8_t velocity)
        {
            midi::File file{1u, 480u, {midi::Track{}}};
            midi::Track guitar;
            guitar.name = "PART GUITAR";
            guitar.events.push_back({0u, 0x90u, 0u, {126u, velocity}});
            for (const uint8_t base : {84u, 96u})
                for (uint32_t tick : {0u, 240u, 480u})
                {
                    guitar.events.push_back(note(tick, base, true));
                    guitar.events.push_back(note(tick + 60u, base, false));
                }
            guitar.events.push_back(note(720u, 126u, false));
            std::stable_sort(guitar.events.begin(), guitar.events.end(),
                             [](const midi::Event &a, const midi::Event &b) { return a.tick < b.tick; });
            file.tracks.push_back(guitar);
            return parseMid(midi::write(file), {}, why);
        };
        for (const uint8_t velocity : {uint8_t(100u), uint8_t(45u)})
        {
            const auto mid = midOf(velocity);
            CHECK(mid && mid->parts.count(Part::Guitar));
            if (!mid || !mid->parts.count(Part::Guitar))
                continue;
            const auto &d = mid->parts.at(Part::Guitar);
            CHECK(at(d[3], 240) && at(d[3], 240)->laned);
            CHECK(at(d[2], 240) && at(d[2], 240)->laned == (velocity == 45u));
        }
    }

    // Charts as written in the wild: a note struck again before the last's
    // end on its tick, no ticks a beat, tempos out of MIDI's reach,
    // negative and wild lengths, a mark and a space before the first
    // section, sections out of order. Each still makes a .mid that reads.
    void chartEdges()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0u, 0u, 0u}});
        midi::Track guitar;
        guitar.name = "PART GUITAR";
        guitar.events = {note(0u, 96u, true), note(480u, 96u, true), note(480u, 96u, false), note(1440u, 96u, false)};
        file.tracks = {tempo, guitar};
        std::string why;
        auto mid = parseMid(midi::write(file), {}, why);
        CHECK(mid && mid->tempos.size() == 1u && mid->tempos[0].usPerBeat == 500000u);
        const Difficulty *x = mid && mid->parts.count(Part::Guitar) ? &mid->parts.at(Part::Guitar)[3] : nullptr;
        CHECK(x && at(*x, 0) && at(*x, 0)->longest() == 480u && at(*x, 480) && at(*x, 480)->longest() == 960u);
        file.division = 0u;
        CHECK(!parseMid(midi::write(file), {}, why));

        const std::string text = "\xef\xbb\xbf [Song]\n{\n  Resolution = 192\n}\n"
                                 "[SyncTrack]\n{\n  0 = B 2000\n  0 = TS 0\n}\n"
                                 "[Events]\n{\n  500 = E \"section B\"\n  0 = E \"section A\"\n}\n"
                                 "[ExpertSingle]\n{\n  0 = N 0 -1\n  192 = N 1 4000000000\n}\n";
        const auto chart = parseChart(text, {}, why);
        CHECK(chart && chart->resolution == 192u && chart->signatures.empty());
        if (!chart)
            return;
        CHECK(chart->tempos.size() == 1u && chart->tempos[0].usPerBeat == 0xffffffu);
        CHECK(chart->sections.size() == 2u && chart->sections[0].second == "A");
        const Difficulty &c = chart->parts.at(Part::Guitar)[3];
        CHECK(at(c, 0) && at(c, 0)->longest() == 0u && at(c, 192) && at(c, 192)->longest() > 0u);
        const auto gh2 = toGh2(*chart, 60.0);
        CHECK(gh2 && midi::parse(gh2->mid));
    }

    // A song of `dir`, its chart its own unless `hash` is given.
    Song song(const std::string &dir, const std::string &name, const std::string &hash = "")
    {
        Song s;
        s.dir = dir;
        s.chart = "notes.chart";
        s.chartMd5 = hash.empty() ? md5::hex(dir) : hash;
        s.tags["name"] = name;
        return s;
    }

    // The catalog's list: by folder then name, a song changed in place, one
    // that stops playing or that a scan did not find gone.
    void catalogOrder()
    {
        Catalog catalog;
        catalog.put(song("/c/b/two", "Zed"));
        catalog.put(song("/c/a/one", "Mid"));
        catalog.put(song("/c/b/three", "<b>Alpha</b>"));
        Snapshot now = catalog.now();
        CHECK(now->size() == 3u && (*now)[0].header == "a" && (*now)[1].name == "Alpha" && (*now)[2].name == "Zed");
        CHECK(catalog.now() == now);
        const std::string id = (*now)[2].id;
        CHECK(id.rfind("cs_", 0u) == 0u && catalog.find(id) && catalog.find(id)->dir == "/c/b/two");
        Song changed = song("/c/b/two", "Zed");
        changed.chartTime = 1;
        catalog.put(changed);
        CHECK(catalog.now() != now && catalog.now()->size() == 3u && catalog.now()->at(2).id == id);
        Song broken = song("/c/a/one", "Mid");
        broken.why = "no audio";
        catalog.put(broken);
        CHECK(catalog.now()->size() == 2u);
        catalog.keepOnly({song("/c/b/three", "Alpha")});
        CHECK(catalog.now()->size() == 1u && !catalog.find(id));
    }

    // Copies of one chart: the first by folder listed, the next once it
    // goes, and each other one a copy of it.
    void catalogCopies()
    {
        const std::vector<Song> songs = {song("/c/x/x/one", "One", "7"), song("/c/x/one", "One", "7"),
                                         song("/c/y/two", "Two")};
        Catalog catalog;
        for (const Song &s : songs)
            catalog.put(s);
        CHECK(catalog.now()->size() == 2u && catalog.now()->at(0).song->dir == "/c/x/one");
        const auto copied = copies(songs);
        CHECK(copied.size() == 1u && copied.count("/c/x/x/one") && copied.at("/c/x/x/one") == "/c/x/one");
        catalog.showCopies(true);
        CHECK(catalog.now()->size() == 3u);
        catalog.showCopies(false);
        CHECK(catalog.now()->size() == 2u);
        catalog.keepOnly({songs[0], songs[2]});
        CHECK(catalog.now()->size() == 2u && catalog.now()->at(0).song->dir == "/c/x/x/one");
    }

    // A copy sits in every folder it is in when listed by folder, so a
    // folder shows all it holds, and once under any other sort.
    void catalogCopiesByFolder()
    {
        Song a = song("/c/rock/one", "One", "7"), b = song("/c/mix/one", "One", "7"), c = song("/c/mix/other", "One", "8");
        a.tags["artist"] = b.tags["artist"] = c.tags["artist"] = "Band";
        Catalog catalog;
        for (const Song &s : {a, b, c})
            catalog.put(s);
        CHECK(catalog.now()->size() == 3u);
        catalog.sortBy(Sort::Artist);
        CHECK(catalog.now()->size() == 2u);
        catalog.sortBy(Sort::Folder);
        CHECK(catalog.now()->size() == 3u);
    }

    // By folder, a section is the folder a song sits in and the one above,
    // under its chart folder: the chart folder's own name for a song
    // straight in it, the deepest of two that hold one another, and two
    // packs' "Songs" kept apart.
    void catalogSections()
    {
        Catalog catalog;
        catalog.chartFolders({"/c/", "/c/rb", "/d"});
        for (const char *dir : {"/c/top", "/c/Pack/one", "/c/Pack/Songs/two", "/c/Other/Songs/two", "/c/A/B/C/three",
                                "/c/rb/Disc/four", "/e/x/five"})
            catalog.put(song(dir, std::string(dir).substr(std::string(dir).rfind('/') + 1u)));
        std::set<std::string> sections;
        for (const Listed &l : *catalog.now())
            sections.insert(l.header + ":" + l.name);
        const std::set<std::string> want = {"c:top",     "Pack:one",  "Pack/Songs:two", "Other/Songs:two",
                                            "B/C:three", "Disc:four", "x:five"};
        CHECK(sections == want);
    }

    // A song with no art and no tags ends its index line on an empty field,
    // and is read back all the same.
    void indexBare()
    {
        const fs::path file = fs::temp_directory_path() / "ghc_index_bare_test.txt";
        Song bare = song("/c/a/bare", "");
        bare.tags.clear();
        bare.why = "no audio";
        bare.reject = Reject::NoAudio;
        writeIndex(file, {song("/c/a/one", "One"), bare, song("/c/a/two", "Two")});
        const std::vector<Song> back = readIndex(file);
        CHECK(back.size() == 3u && back[1].dir == "/c/a/bare" && back[1].art.empty() && back[2].dir == "/c/a/two");
        CHECK(back.size() == 3u && back[1].reject == Reject::NoAudio && back[0].reject == Reject::None);
        fs::remove(file);
    }

    // badsongs.txt: the songs that don't play under their kind's heading,
    // in Reject's order, the why only where the heading doesn't say it;
    // no song that plays, nor a copy.
    void badSongs()
    {
        const fs::path file = fs::temp_directory_path() / "ghc_badsongs_test.txt";
        Song audio = song("/c/a/audio", ""), chart = song("/c/a/chart", ""), drums = song("/c/a/drums", "");
        audio.why = "song.mp3 is MPEG-4 audio", audio.reject = Reject::BadAudio;
        chart.why = "notes.mid: not a MIDI file", chart.reject = Reject::BadChart;
        drums.why = "no 5-fret guitar or bass part", drums.reject = Reject::NoPart;
        writeBadSongs(file, {audio, song("/c/a/one", "One", "7"), drums, chart, song("/c/b/one", "One", "7")});
        std::ifstream in(file);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close(); // Windows removes no file held open
        const std::string want = "\nThese charts are corrupted and can't be read.\n/c/a/chart\n    notes.mid: not a MIDI file\n"
                                 "\nThese charts have no part GH Classic plays (guitar, co-op guitar, rhythm or bass).\n/c/a/drums\n"
                                 "\nThese songs' audio is corrupted or in a format GH Classic can't play.\n/c/a/audio\n"
                                 "    song.mp3 is MPEG-4 audio\n";
        CHECK(text.size() > want.size() && text.compare(text.size() - want.size(), want.size(), want) == 0);
        CHECK(text.find("/c/a/one") == std::string::npos && text.find("/c/b/one") == std::string::npos);
        fs::remove(file);
    }

    // Every stem name Clone Hero and YARG play, any case and format, in
    // kStems' order; what else charts carry (a preview, a Mac's ._ files,
    // a download's second copy, censored takes) left out. Then which
    // stems go with the played part.
    void stems()
    {
        const fs::path dir = fs::temp_directory_path() / "ghc_stems_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        std::ofstream(dir / "song.ini") << "[Song]\nName = Stems\n";
        std::ofstream(dir / "notes.chart") << kOneGem;
        const std::map<std::string, std::string> heads = {
            {".ogg", kVorbisHead},
            {".opus", "OggS" + std::string(24, '\0') + "OpusHead"},
            {".mp3", "ID3"},
            {".wav", "RIFF" + std::string(4, '\0') + "WAVE"},
            {".aiff", "FORM" + std::string(4, '\0') + "AIFF"},
        };
        const std::vector<std::string> played = {
            "song.ogg",       "Guitar.opus",          "rhythm.mp3",         "bass.wav",      "keys.aiff",
            "vocals.ogg",     "vocals_1.ogg",         "VOCALS_2.ogg",       "drums.ogg",     "drums_1.ogg",
            "drums_2.opus",   "drums_3.mp3",          "Drums_4.wav",        "crowd.ogg",     "vocals_explicit.ogg",
            "vocals_1_explicit.ogg", "vocals_2_explicit.ogg", "crowd_explicit.ogg",
        };
        for (const std::string &file : played)
            std::ofstream(dir / file, std::ios::binary) << heads.at(file.substr(file.rfind('.')));
        for (const char *file : {"preview.ogg", "._drums_1.ogg", "song (1).ogg", "song_clean.ogg", "center.ogg"})
            std::ofstream(dir / file, std::ios::binary) << kVorbisHead;
        const Song song = read(dir, "notes.chart");
        CHECK(song.why.empty());
        CHECK(song.stems == played);
        fs::remove_all(dir);

        CHECK(partStem("drums.ogg", "drums") && partStem("drums_1.ogg", "drums") && partStem("DRUMS_4.Opus", "drums"));
        CHECK(partStem("vocals_2.ogg", "vocals") && partStem("vocals_1_explicit.ogg", "vocals"));
        CHECK(partStem("guitar.ogg", "guitar") && !partStem("song.ogg", "guitar") && !partStem("bass.ogg", "guitar"));
        CHECK(!partStem("drumsx.ogg", "drums") && !partStem("rhythm.ogg", "drums"));
    }

    // Folders that hold one another are walked once, so no song is found twice.
    void overlappingRoots()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_overlap_test";
        fs::remove_all(root);
        fs::create_directories(root / "rock" / "Song");
        std::ofstream(root / "rock" / "Song" / "song.ini") << "[Song]\nName = Once\n";
        std::ofstream(root / "rock" / "Song" / "notes.chart") << kOneGem;
        std::ofstream(root / "rock" / "Song" / "guitar.ogg", std::ios::binary) << kVorbisHead;
        const std::vector<Song> scanned = scan({root, root / "rock", root / "rock" / "", root}, {});
        CHECK(scanned.size() == 1u && copies(scanned).empty());
        fs::remove_all(root);
    }

    // `files` written as an .sng, as SngFileFormat's packer does: song.ini's
    // tags as its metadata, each other file after the index, masked.
    void pack(const fs::path &out, const Tags &tags, const std::map<std::string, std::string> &files)
    {
        std::string b = "SNGPKG", meta;
        // `v` little-endian, `bytes` long, onto `to` (b unless named).
        const auto u = [&](uint64_t v, int bytes, std::string *to = nullptr)
        {
            for (int i = 0; i < bytes; ++i)
                (to ? *to : b) += static_cast<char>((v >> (8 * i)) & 0xffu);
        };
        const uint8_t mask[16] = {0x13, 0x37, 0xa5, 0x5a, 0x01, 0xfe, 0x80, 0x7f, 9, 8, 7, 6, 5, 4, 3, 2};
        u(1, 4);
        b.append(reinterpret_cast<const char *>(mask), 16);
        for (const auto &[key, value] : tags)
            for (const std::string *s : {&key, &value})
            {
                u(s->size(), 4, &meta);
                meta += *s;
            }
        u(meta.size() + 8u, 8);
        u(tags.size(), 8);
        b += meta;
        size_t index = 8u;
        for (const auto &[name, contents] : files)
            index += 17u + name.size();
        u(index, 8);
        u(files.size(), 8);
        // The data's start: past the index and the data's own length.
        uint64_t at = b.size() + (index - 8u) + 8u;
        std::string data;
        for (const auto &[name, contents] : files)
        {
            b += static_cast<char>(name.size());
            b += name;
            u(contents.size(), 8);
            u(at, 8);
            at += contents.size();
            for (size_t i = 0; i < contents.size(); ++i)
                data += static_cast<char>(contents[i] ^ mask[i & 15u] ^ static_cast<uint8_t>(i & 0xffu));
        }
        u(data.size(), 8);
        b += data;
        std::ofstream(out, std::ios::binary) << b;
    }

    // Each sort's tiers: their headers, in rank then name order, the song
    // without the tag last.
    void catalogSort()
    {
        Song a = song("/c/one/a", "Beta"), b = song("/c/two/b", "alpha"), c = song("/c/one/c", "9 Lives");
        a.tags["artist"] = "Zed", b.tags["artist"] = "abba";
        a.length = 200.0, b.length = 61.0, c.length = 700.0;
        a.tags["diff_guitar"] = "4", b.tags["diff_guitar"] = "2", c.tags["diff_guitar"] = "-1";
        a.parts = b.parts = c.parts = 1u << int(Part::Guitar);
        Catalog catalog;
        for (const Song &s : {a, b, c})
            catalog.put(s);
        const auto headers = [&](Sort sort)
        {
            catalog.sortBy(sort);
            std::string out;
            for (const Listed &l : *catalog.now())
                out += l.header + "/" + l.name + ";";
            return out;
        };
        CHECK(headers(Sort::Folder) == "one/9 Lives;one/Beta;two/alpha;");
        CHECK(headers(Sort::Artist) == "abba/alpha;Zed/Beta;UNKNOWN ARTIST/9 Lives;");
        CHECK(headers(Sort::Title) == "#/9 Lives;A/alpha;B/Beta;");
        CHECK(headers(Sort::Length) == "1:00 - 2:00/alpha;3:00 - 4:00/Beta;10:00+/9 Lives;");
        CHECK(headers(Sort::Difficulty) == "DIFFICULTY 2/alpha;DIFFICULTY 4/Beta;NO DIFFICULTY/9 Lives;");
    }

    // A song as a folder and as an .sng read the same, and its files come
    // out of the package as they went in.
    void package()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_sng_test";
        fs::remove_all(root);
        fs::create_directories(root / "Folder");
        const std::string ini = "[song]\nname = Packed\nartist = Someone\ndelay = 0\n";
        const std::string chart = "[Song]\n{\n  Resolution = 192\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                  "[ExpertSingle]\n{\n  0 = N 0 0\n  192 = N 1 0\n  384 = N 7 0\n}\n";
        // An Ogg page that names Vorbis, long enough to cross the mask's
        // 16 and 256.
        std::string ogg = kVorbisHead;
        for (int i = 0; ogg.size() < 700u; ++i)
            ogg += static_cast<char>(i * 31);
        std::ofstream(root / "Folder" / "song.ini") << ini;
        std::ofstream(root / "Folder" / "notes.chart", std::ios::binary) << chart;
        std::ofstream(root / "Folder" / "song.ogg", std::ios::binary) << ogg;
        pack(root / "Packed.sng", parseIni(ini), {{"notes.chart", chart}, {"song.ogg", ogg}});

        const Song folder = read(root / "Folder", "notes.chart"), packed = read(root / "Packed.sng", "");
        CHECK(folder.why.empty() && packed.why.empty());
        CHECK(packed.tags == folder.tags && packed.stems == folder.stems && packed.parts == folder.parts);
        CHECK(packed.chart == "notes.chart" && packed.chartMd5 == folder.chartMd5 && packed.chartMd5 == md5::hex(chart));
        CHECK(songFile(root / "Packed.sng" / "song.ogg") == ogg && songFile(root / "Folder" / "song.ogg") == ogg);
        CHECK(title(packed) == "Packed");
        const std::vector<Song> scanned = scan({root}, {});
        CHECK(scanned.size() == 2u && copies(scanned).size() == 1u);
        fs::remove_all(root);
    }

    // RFC 1321's own examples, and one past a block's padding.
    void md5Vectors()
    {
        CHECK(md5::hex("") == "d41d8cd98f00b204e9800998ecf8427e");
        CHECK(md5::hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
        CHECK(md5::hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
        CHECK(md5::hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
              "57edf4a22be3c955ac49da2e2107b67a");
    }

    // A folder as chart sites spell it, its files in any case; an .sng that
    // claims a file past its end is none.
    void spelled()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_spelled_test";
        fs::remove_all(root);
        fs::create_directories(root / "Song");
        std::ofstream(root / "Song" / "Song.ini") << "[Song]\nName = Spelled\n";
        std::ofstream(root / "Song" / "NOTES.chart") << kOneGem;
        std::ofstream(root / "Song" / "Guitar.ogg", std::ios::binary) << kVorbisHead;
        std::ofstream(root / "Song" / "Album.PNG") << "png";
        const std::vector<Song> scanned = scan({root}, {});
        CHECK(scanned.size() == 1u);
        if (scanned.size() == 1u)
        {
            const Song &s = scanned[0];
            CHECK(s.why.empty() && s.chart == "NOTES.chart" && tag(s.tags, "name") == "Spelled" && s.iniTime != 0);
            CHECK(s.stems == std::vector<std::string>{"Guitar.ogg"} && s.art == "Album.PNG");
            CHECK(songFile(songPath(s, s.art)) == "png");
        }
        pack(root / "Cut.sng", {}, {{"notes.chart", "[Song]\n{\n}\n"}});
        fs::resize_file(root / "Cut.sng", fs::file_size(root / "Cut.sng") - 4u);
        std::string why;
        CHECK(!sng::read(root / "Cut.sng", why) && !why.empty());
        fs::remove_all(root);
    }

    // An artist spelled two ways is one section, as its first song spells
    // it.
    void catalogSpelling()
    {
        Song a = song("/c/x/a", "One"), b = song("/c/x/b", "Two"), c = song("/c/x/c", "Three");
        a.tags["artist"] = "Metallica", b.tags["artist"] = "METALLICA", c.tags["artist"] = "metallica";
        Catalog catalog;
        for (const Song &s : {a, b, c})
            catalog.put(s);
        catalog.sortBy(Sort::Artist);
        const Snapshot now = catalog.now();
        CHECK(now->size() == 3u &&
              std::all_of(now->begin(), now->end(), [&](const Listed &l) { return l.header == now->front().header; }));
    }

    // .chart's Offset as song.ini's delay where it has none, never over it.
    void offsetLead()
    {
        const fs::path root = fs::temp_directory_path() / "ghc_offset_test";
        fs::remove_all(root);
        const std::string chart = "[Song]\n{\n  Resolution = 192\n  Offset = 0.25\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                  "[ExpertSingle]\n{\n  0 = N 0 0\n}\n";
        for (const auto &[name, ini] : {std::pair{"None", "[song]\nname = A\n"}, std::pair{"Zero", "[song]\ndelay = 0\n"},
                                        std::pair{"Own", "[song]\ndelay = 100\n"}})
        {
            fs::create_directories(root / name);
            std::ofstream(root / name / "song.ini") << ini;
            std::ofstream(root / name / "notes.chart") << chart;
        }
        CHECK(number(read(root / "None", "notes.chart").tags, "delay") == 250);
        CHECK(number(read(root / "Zero", "notes.chart").tags, "delay") == 250);
        CHECK(number(read(root / "Own", "notes.chart").tags, "delay") == 100);
        fs::remove_all(root);
    }

    // Every song folder under `root`, read and tallied.
    void library(const fs::path &root)
    {
        std::map<std::string, int> tally;
        std::error_code error;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
             it != end; it.increment(error))
        {
            if (error || !it->is_regular_file() || it->path().filename() != "song.ini")
                continue;
            const fs::path dir = it->path().parent_path();
            const Tags tags = parseIni(songFile(it->path()));
            std::optional<Chart> chart;
            std::string why = "no notes.mid or notes.chart";
            for (const char *name : {"notes.mid", "notes.chart"})
                if (fs::exists(dir / name))
                {
                    chart = parseSongChart(name, songFile(dir / name), options(tags), why);
                    ++tally[fs::path(name).extension().string().substr(1u)];
                    break;
                }
            ++tally["songs"];
            if (chart && !chart->parts.count(Part::Guitar))
                why = "no guitar part";
            if (!chart || !chart->parts.count(Part::Guitar))
            {
                ++tally["unreadable"];
                std::cout << "  " << fs::relative(dir, root).string() << ": " << why << "\n";
                continue;
            }
            const Difficulty &x = chart->parts.at(Part::Guitar)[3];
            if (x.gems.empty())
                ++tally["no expert"];
            bool taps = false, opens = false, forced = false, split = false, extended = false;
            for (size_t i = 0; i < x.gems.size(); ++i)
            {
                const Gem &g = x.gems[i];
                taps |= g.kind == Kind::Tap;
                opens |= (g.lanes & (1u << kOpen)) != 0u;
                forced |= g.forced && g.kind != Kind::Tap;
                split |= g.split();
                // Held past the next gem's start.
                extended |= i + 1u < x.gems.size() && g.tick + g.longest() > x.gems[i + 1u].tick;
            }
            tally["with taps"] += taps;
            tally["with opens"] += opens;
            tally["with forced"] += forced;
            tally["with split chords"] += split;
            tally["with extended sustains"] += extended;
        }
        std::cout << root.string() << "\n";
        for (const auto &[what, n] : tally)
            std::cout << "  " << what << ": " << n << "\n";
    }

    // The scan of `root`, its index read back as written, and a rescan from
    // it, which reads nothing again.
    void scanned(const fs::path &root, const fs::path &out, unsigned workers)
    {
        using clock = std::chrono::steady_clock;
        const auto start = clock::now();
        const std::vector<Song> songs = scan({root}, {}, workers);
        const auto first = clock::now();
        std::map<std::string, int> why;
        for (const Song &s : songs)
            ++why[s.why.empty() ? "plays" : s.why.substr(0, s.why.find(':') == std::string::npos ? s.why.size() : s.why.find(':') + 1u)];
        std::cout << "scan " << root.string() << ": " << songs.size() << " songs, "
                  << std::chrono::duration<double>(first - start).count() << " s\n";
        for (const auto &[what, n] : why)
            std::cout << "  " << what << ": " << n << "\n";
        const fs::path index = out / "index.txt";
        writeIndex(index, songs);
        writeBadSongs(out / "badsongs.txt", songs);
        writeSongList(out / "songs.csv", songs, copies(songs));
        const std::vector<Song> back = readIndex(index);
        CHECK(back.size() == songs.size());
        for (size_t i = 0; i < std::min(back.size(), songs.size()); ++i)
        {
            CHECK(back[i].dir == songs[i].dir);
            CHECK(back[i].why == songs[i].why && back[i].parts == songs[i].parts && back[i].stems == songs[i].stems);
            CHECK(back[i].tags == songs[i].tags && back[i].chartMd5 == songs[i].chartMd5);
        }
        const std::vector<Song> again = scan({root}, back, workers);
        std::cout << "  rescan: " << std::chrono::duration<double>(clock::now() - first).count() << " s\n";
        CHECK(again.size() == songs.size());
    }
}

int main(int argc, char **argv)
{
    mixedRates();
    chartTypes();
    heldPastNext();
    midTypes();
    midMarkers();
    lanes();
    chartEdges();
    catalogOrder();
    catalogCopies();
    catalogCopiesByFolder();
    catalogSections();
    stems();
    overlappingRoots();
    indexBare();
    badSongs();
    md5Vectors();
    catalogSort();
    package();
    spelled();
    catalogSpelling();
    offsetLead();
    // --out <dir>: where each scan's index and badsongs.txt go. --scan:
    // only the scan, each chart read once, as the game would (a network
    // drive's library is slow enough to read once). --workers <n>: folders
    // scanned at once. Each applies to the folders named after it.
    fs::path out = fs::temp_directory_path();
    bool scanOnly = false;
    unsigned workers = 32u;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (std::string(argv[i]) == "--workers" && i + 1 < argc)
            workers = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::string(argv[i]) == "--scan")
            scanOnly = true;
        else
        {
            if (!scanOnly)
                library(argv[i]);
            scanned(argv[i], out, workers);
        }
    if (s_failed)
        std::cerr << s_failed << " failed" << std::endl;
    else
        std::cout << "custom_chart_test: all passed" << std::endl;
    return s_failed ? 1 : 0;
}
