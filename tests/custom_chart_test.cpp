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
#include "custom/song_facts.h"
#include "custom/song_ini.h"
#include "custom/stage/stage.h"
#include "formats/md5.h"
#include "formats/midi.h"
#include "formats/sng.h"

#include <algorithm>
#include <chrono>
#include <cstring>
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

        // An open with frets is an open chord: the frets and the open, a
        // chord, so never a natural HOPO. GH2 is given its frets, the open
        // beside them, its kind the chart's.
        const auto openChord = parseChart(
            "[ExpertSingle]\n{\n  0 = N 0 0\n  48 = N 2 0\n  48 = N 7 0\n  96 = N 0 0\n  96 = N 1 0\n  96 = N 7 0\n}\n", {}, why);
        CHECK(openChord && openChord->parts.count(Part::Guitar));
        if (openChord && openChord->parts.count(Part::Guitar))
        {
            const Difficulty &o = openChord->parts.at(Part::Guitar)[3];
            CHECK(at(o, 48) && at(o, 48)->lanes == ((1u << 2) | (1u << kOpen)) && at(o, 48)->kind == Kind::Strum);
            CHECK(at(o, 96) && at(o, 96)->lanes == (0x3u | (1u << kOpen)));
            const auto converted = toGh2(*openChord, 0.0);
            CHECK(converted);
            if (converted)
            {
                const auto &k = converted->gems[3];
                CHECK(k.count(0) && !k.at(0).openChord);
                CHECK(k.count(120) && k.at(120).openChord && !k.at(120).open && k.at(120).frets == 0x4u &&
                      k.at(120).decided && !k.at(120).hopo);
                CHECK(k.count(240) && k.at(240).openChord && k.at(240).frets == 0x3u);
            }
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
        // Tempo, PART GUITAR, EVENTS, then the band's (custom/stage/stage.h).
        CHECK(back && back->division == 480u && back->tracks.size() == 8u);
        if (!back || back->tracks.size() != 8u)
            return;
        CHECK(back->tracks[3].name == "BAND DRUMS" && back->tracks[4].name == "BAND BASS" &&
              back->tracks[5].name == "BAND SINGER" && back->tracks[6].name == "TRIGGERS" &&
              back->tracks[7].name == "GH1 UPCOMING");
        std::map<uint8_t, std::vector<uint32_t>> ons;
        for (const midi::Event &e : back->tracks[1].events)
            if (midi::kind(e) == midi::kNoteOn && e.data[0] >= 60u) // past the hand's places
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

    // A .mid at 480 a beat and 120 BPM with one gem on PART GUITAR, then
    // `more`; a .chart at 192 and 120 BPM with one; a chart at 480 and 120
    // BPM with nothing; ticks the same at 480.
    std::vector<uint8_t> oneGemMid(const std::vector<midi::Track> &more)
    {
        midi::Track tempo, guitar;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0x07u, 0xa1u, 0x20u}});
        guitar.name = "PART GUITAR";
        guitar.events = {note(0u, 96u, true), note(60u, 96u, false)};
        midi::File file{1u, 480u, {tempo, guitar}};
        file.tracks.insert(file.tracks.end(), more.begin(), more.end());
        return midi::write(file);
    }
    const std::string kOneGemChart = "[Song]\n{\n  Resolution = 192\n}\n[SyncTrack]\n{\n  0 = B 120000\n}\n"
                                     "[ExpertSingle]\n{\n  0 = N 0 0\n}\n";
    Chart chart480()
    {
        Chart chart;
        chart.resolution = 480u;
        chart.tempos = {Tempo{}};
        return chart;
    }
    const auto kSameTicks = [](uint32_t t) { return t; };

    // The drums' expert hits and the singing, from a .mid's PART DRUMS and
    // PART VOCALS (its phrases, else its notes' runs) and a .chart's
    // [ExpertDrums] and lyrics.
    void bandParts()
    {
        midi::Track drums, vocals;
        drums.name = "PART DRUMS";
        for (const auto &[tick, pitch] : std::vector<std::pair<uint32_t, uint8_t>>{{0u, 96u}, {480u, 97u}, {960u, 95u}, {960u, 99u}, {960u, 84u}})
        {
            drums.events.push_back(note(tick, pitch, true));
            drums.events.push_back(note(tick + 60u, pitch, false));
        }
        vocals.name = "PART VOCALS";
        // Notes only: two runs, a beat and four beats apart.
        for (const uint32_t tick : {0u, 240u, 480u, 2880u, 3120u})
        {
            vocals.events.push_back(note(tick, 60u, true));
            vocals.events.push_back(note(tick + 120u, 60u, false));
        }
        std::string why;
        const auto mid = parseMid(oneGemMid({drums, vocals}), {}, why);
        CHECK(mid && mid->kicks == std::vector<uint32_t>({0u, 960u}) &&
              mid->drums == std::vector<uint32_t>({0u, 480u, 960u, 960u}) && mid->snares == std::vector<uint32_t>({480u}));
        CHECK(mid && mid->vocals.size() == 2u && mid->vocals[0].tick == 0u && mid->vocals[1].tick == 2880u);

        // Its phrases where it has them.
        vocals.events.push_back(note(100u, 105u, true));
        vocals.events.push_back(note(700u, 105u, false));
        std::stable_sort(vocals.events.begin(), vocals.events.end(),
                         [](const midi::Event &a, const midi::Event &b) { return a.tick < b.tick; });
        const auto phrased = parseMid(oneGemMid({drums, vocals}), {}, why);
        CHECK(phrased && phrased->vocals.size() == 1u && phrased->vocals[0].tick == 100u && phrased->vocals[0].length == 600u);

        const std::string text = kOneGemChart +
                                 "[Events]\n{\n  0 = E \"phrase_start\"\n  96 = E \"lyric la\"\n  384 = E \"phrase_end\"\n}\n"
                                 "[ExpertDrums]\n{\n  0 = N 0 0\n  192 = N 2 0\n  384 = N 32 0\n  384 = N 66 0\n}\n";
        const auto chart = parseChart(text, {}, why);
        CHECK(chart && chart->kicks == std::vector<uint32_t>({0u, 384u}) && chart->drums.size() == 3u &&
              chart->snares.empty());
        CHECK(chart && chart->vocals.size() == 1u && chart->vocals[0].tick == 0u && chart->vocals[0].length == 384u);
        CHECK(chart && chart->keys.empty() && chart->stage.at("EVENTS").cues.size() == 3u &&
              chart->stage.at("EVENTS").cues[1].text == "lyric la");
        const auto keyed = parseChart(text + "[ExpertKeyboard]\n{\n  0 = N 1 0\n  0 = N 3 0\n  192 = N 7 0\n}\n", {}, why);
        CHECK(keyed && keyed->keys == std::vector<uint32_t>({0u, 0u}));
        // Lines out of order sorted, a note or phrase that doesn't read none.
        const auto messy = parseChart(kOneGemChart + "[SyncTrack]\n{\n  768 = TS 3\n  0 = TS 4\n}\n[ExpertSingle]\n{\n"
                                                     "  384 = E solo\n  480 = E soloend\n  0 = E solo\n  96 = E soloend\n"
                                                     "  192 = N x 0\n  192 = S x 0\n}\n",
                                      {}, why);
        const Difficulty *expert = messy ? &messy->parts.at(Part::Guitar)[3] : nullptr;
        CHECK(messy && messy->signatures.size() == 2u && messy->signatures[0].first == 0u);
        CHECK(expert && expert->solos.size() == 2u && expert->solos[0].tick == 0u && expert->gems.size() == 1u &&
              expert->faceOff[0].empty());
    }

    std::vector<uint32_t> notesOf(const midi::Track &track, uint8_t from, uint8_t to);

    // The sung syllables: each lyric with its note, its markup off, a
    // slide's '+' adding its note to the syllable before; one with no note
    // held to the next, a beat at most; .chart's lyric events alike.
    void syllables()
    {
        midi::Track vocals;
        vocals.name = "PART VOCALS";
        const auto lyric = [](uint32_t tick, const std::string &text)
        { return midi::Event{tick, midi::kMeta, midi::kLyric, {text.begin(), text.end()}}; };
        vocals.events = {lyric(0u, "Hel-"),  note(0u, 60u, true),   note(240u, 60u, false),
                         lyric(240u, "lo#"), note(240u, 62u, true), note(480u, 62u, false),
                         lyric(480u, "+"),   note(480u, 64u, true), note(960u, 64u, false),
                         lyric(1920u, "world")};
        std::string why;
        const auto mid = parseMid(oneGemMid({vocals}), {}, why);
        CHECK(mid && mid->syllables.size() == 3u);
        if (mid && mid->syllables.size() == 3u)
        {
            CHECK(mid->syllables[0].text == "Hel" && mid->syllables[0].length == 240u);
            CHECK(mid->syllables[1].text == "lo" && mid->syllables[1].length == 720u); // through the slide
            CHECK(mid->syllables[2].text == "world" && mid->syllables[2].length == 480u);
        }
        const auto chart = parseChart(kOneGemChart + "[Events]\n{\n  0 = E \"lyric Oh-\"\n  96 = E \"lyric yeah\"\n}\n", {}, why);
        CHECK(chart && chart->syllables.size() == 2u && chart->syllables[0].text == "Oh" &&
              chart->syllables[0].length == 96u && chart->syllables[1].length == 192u);
        // A slide's '+' holds the syllable before through it, in either
        // format (custom/chart_rules.h's addLyric).
        const auto slid = parseChart(
            kOneGemChart + "[Events]\n{\n  0 = E \"lyric Oh\"\n  288 = E \"lyric +\"\n  480 = E \"lyric yeah\"\n}\n", {}, why);
        CHECK(slid && slid->syllables.size() == 2u && slid->syllables[0].length == 288u);
        // A slide before the syllable written last holds nothing.
        const auto back = parseChart(kOneGemChart + "[Events]\n{\n  480 = E \"lyric yeah\"\n  288 = E \"lyric +\"\n}\n", {}, why);
        CHECK(back && back->syllables.size() == 1u && back->syllables[0].length == 192u);
        // Lyrics as text events with no notes: PART VOCALS' alone, not
        // HARM1's after them.
        midi::Track texts, harmony;
        texts.name = "PART VOCALS";
        texts.events = {midi::text(0u, "la"), midi::text(480u, "di")};
        harmony.name = "HARM1";
        harmony.events = {lyric(240u, "oh")};
        const auto texted = parseMid(oneGemMid({texts, harmony}), {}, why);
        CHECK(texted && texted->syllables.size() == 2u && !texted->vocals.empty());
    }

    // A blink track as GH2's: its header, Blink with a key, Eat and Ox
    // empty, a tail.
    std::vector<uint8_t> blinkTrack()
    {
        std::vector<uint8_t> b = {'F', 'A', 'C', 'E'};
        const auto u32 = [&](uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                b.push_back(static_cast<uint8_t>(v >> (8 * i)));
        };
        const auto u16 = [&](uint16_t v) { b.push_back(static_cast<uint8_t>(v)), b.push_back(static_cast<uint8_t>(v >> 8)); };
        const auto text = [&](const std::string &s) { u32(static_cast<uint32_t>(s.size())), b.insert(b.end(), s.begin(), s.end()); };
        u32(1200u), u16(0u), text("Harmonix"), u16(0u), text("Karaoke Revolution Vol 4");
        b.insert(b.end(), 12u, 0u);
        text("blinkTrack"), u16(0u), u32(0u), u16(0u), u32(3u);
        for (const char *name : {"Blink", "Eat", "Ox"})
        {
            b.insert(b.end(), 8u, 0u);
            text(name);
            b.insert(b.end(), 8u, 0u);
            const bool blink = std::string(name) == "Blink";
            u32(blink ? 1u : 0u);
            if (blink)
                b.insert(b.end(), 18u, 7u);
        }
        b.insert(b.end(), 14u, 9u);
        return b;
    }

    // The mouth: each syllable's consonants and vowel as GH2's visemes, in
    // the blink track's curves, its blinks and tail kept, its size its own;
    // GH1's singer's open mouth the shapes' spans, near ones as one; a chart
    // with nothing sung sung on each beat the band plays.
    void lipSync()
    {
        const auto shapes = mouthShapes({{1.0, 1.5, "bah"}, {2.0, 2.4, "see"}});
        const auto has = [&](Viseme viseme, double at)
        {
            return std::any_of(shapes.begin(), shapes.end(),
                               [&](const Shape &s) { return s.viseme == viseme && s.start <= at && s.end >= at; });
        };
        CHECK(has(Viseme::Bump, 1.01) && has(Viseme::Ox, 1.25) && !has(Viseme::Ox, 2.2)); // not in "see"
        CHECK(has(Viseme::Size, 2.01) && has(Viseme::Eat, 2.2));

        const std::vector<uint8_t> blink = blinkTrack();
        const std::vector<uint8_t> voc = withMouth(blink, shapes);
        const auto curves = vocCurves(voc);
        CHECK(curves.size() == 3u && curves[0].first == "Blink" && curves[0].second == 1u && curves[1].first == "Eat" &&
              curves[1].second == 4u && curves[2].first == "Ox" && curves[2].second == 4u);
        uint32_t size = 0u;
        std::memcpy(&size, voc.data() + 10u + 4u + 8u + 2u + 4u + 24u + 12u + 4u + 10u + 2u, 4u);
        CHECK(size == voc.size() && std::equal(blink.end() - 14, blink.end(), voc.end() - 14));
        CHECK(withMouth({1u, 2u, 3u}, shapes) == std::vector<uint8_t>({1u, 2u, 3u}));
        // Its open mouth read back, as GH1's singer takes a GH2 song's.
        const auto back = vocMouthOpen(voc);
        CHECK(back.size() == 2u && back[0].first > 1.0f && back[0].first < 1.1f && back[1].second > 2.3f &&
              back[1].second <= 2.4f);
        CHECK(vocMouthOpen(blink).empty());

        const auto open = mouthOpen({{1.0, 1.2, Viseme::Eat, 1.0f}, {1.22, 1.5, Viseme::Ox, 1.0f}, {3.0, 3.1, Viseme::Eat, 1.0f}});
        CHECK(open.size() == 2u && open[0].first == 1.0f && open[0].second == 1.5f);

        // Nothing sung: one on each beat the band plays, a half beat long.
        Chart chart = chart480();
        Difficulty lead;
        for (uint32_t t = 0; t < 8u * 480u; t += 480u)
            lead.gems.push_back({t, 1u});
        chart.parts[Part::Guitar][3] = lead;
        const auto gh2 = toGh2(chart, 0.0);
        CHECK(gh2 && gh2->sung.size() >= 6u && gh2->sung[0].end - gh2->sung[0].start == 0.25);
    }

    // The drums' crashes: pro drums' cymbals (a tom marker makes a pad a
    // tom), a plain or five-lane chart's green or orange, .chart's cymbal
    // markers; the drummer's animation notes over any.
    void drumPieces()
    {
        const auto mid = [](std::vector<std::pair<uint32_t, uint8_t>> hits, std::vector<std::pair<uint32_t, uint8_t>> marks)
        {
            midi::Track drums;
            drums.name = "PART DRUMS";
            for (const auto &[tick, pitch] : marks)
            {
                drums.events.push_back(note(tick, pitch, true));
                drums.events.push_back(note(tick + 60u, pitch, false));
            }
            for (const auto &[tick, pitch] : hits)
            {
                drums.events.push_back(note(tick, pitch, true));
                drums.events.push_back(note(tick + 30u, pitch, false));
            }
            std::stable_sort(drums.events.begin(), drums.events.end(),
                             [](const midi::Event &a, const midi::Event &b) { return a.tick < b.tick; });
            std::string why;
            return parseMid(oneGemMid({drums}), {}, why);
        };
        // Pro: blue under a tom marker a tom; green and the second blue
        // crashes.
        const auto pro = mid({{0u, 98u}, {480u, 99u}, {960u, 100u}, {1440u, 99u}}, {{480u, 111u}});
        CHECK(pro && pro->crashes == std::vector<uint32_t>({960u, 1440u}));
        // Plain four-lane: green the crash, blue none.
        const auto plain = mid({{0u, 98u}, {480u, 99u}, {960u, 100u}}, {});
        CHECK(plain && plain->crashes == std::vector<uint32_t>({960u}));
        // Five-lane: orange (100) the crash, its green (101) a tom.
        const auto five = mid({{0u, 98u}, {480u, 101u}, {960u, 100u}}, {});
        CHECK(five && five->crashes == std::vector<uint32_t>({960u}) && five->drums.size() == 3u);
        // Animation notes over the gems: 26 the snare, 36 a crash.
        const auto anim = mid({{0u, 98u}, {0u, 97u}, {960u, 100u}, {240u, 26u}, {480u, 30u}, {720u, 36u}}, {});
        CHECK(anim && anim->snares == std::vector<uint32_t>({240u}) && anim->crashes == std::vector<uint32_t>({720u}));

        std::string why;
        // .chart pro: pads toms unless marked; green marked.
        const auto chartPro = parseChart(kOneGemChart + "[ExpertDrums]\n{\n  0 = N 2 0\n  0 = N 66 0\n  192 = N 3 0\n"
                                                        "  384 = N 4 0\n  384 = N 68 0\n}\n",
                                         {}, why);
        CHECK(chartPro && chartPro->crashes == std::vector<uint32_t>({384u}));
        const auto chartPlain =
            parseChart(kOneGemChart + "[ExpertDrums]\n{\n  0 = N 2 0\n  192 = N 3 0\n  384 = N 4 0\n}\n", {}, why);
        CHECK(chartPlain && chartPlain->crashes == std::vector<uint32_t>({384u}));

        // The stage's crash on the chart's, none at the sections then.
        Chart chart = chart480();
        chart.sections = {{0u, "Chorus"}, {1920u, "Chorus 2"}};
        chart.drums = {0u, 960u};
        chart.crashes = {960u};
        Difficulty lead;
        for (uint32_t t = 0; t < 8u * 480u; t += 480u)
            lead.gems.push_back({t, 1u});
        midi::Track g, ev;
        const auto band = addStage({chart, lead, kSameTicks, 8u * 480u}, g, ev);
        CHECK(!band.empty() && notesOf(band[0], 37u, 37u) == std::vector<uint32_t>({960u}));
    }

    // Each stage track's texts and animation notes as written; five-lane
    // green a drum hit; HARM1 the singing where PART VOCALS has none; keys
    // by PART KEYS' notes.
    void stageTracks()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo, guitar, drums, harmony, keys, venue, events;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0x07u, 0xa1u, 0x20u}});
        guitar.name = "PART GUITAR";
        guitar.events = {midi::text(0u, "[map HandMap_Solo]"), note(0u, 45u, true), note(0u, 96u, true),
                         note(60u, 96u, false),               note(480u, 45u, false)};
        drums.name = "PART DRUMS";
        drums.events = {note(0u, 24u, true), note(0u, 101u, true), note(60u, 24u, false), note(60u, 101u, false)};
        harmony.name = "HARM1";
        harmony.events = {midi::text(0u, "[play]"), note(0u, 105u, true), note(0u, 50u, true), note(240u, 50u, false),
                          note(480u, 105u, false)};
        keys.name = "PART KEYS";
        keys.events = {note(0u, 30u, true), note(60u, 30u, false)};
        venue.name = "VENUE";
        venue.events = {midi::text(0u, "[lighting (verse)]"), note(0u, 110u, true), note(120u, 110u, false)};
        events.name = "EVENTS";
        events.events = {note(0u, 24u, true), midi::text(0u, " [crowd_clap] "), note(30u, 24u, false)};
        file.tracks = {tempo, guitar, drums, harmony, keys, venue, events};
        std::string why;
        const auto mid = parseMid(midi::write(file), {}, why);
        CHECK(mid && mid->drums == std::vector<uint32_t>({0u}));
        CHECK(mid && mid->vocals.size() == 1u && mid->vocals[0].length == 480u);
        CHECK(mid && mid->keys.empty()); // only an animation note
        if (!mid)
            return;
        const auto &s = mid->stage;
        CHECK(s.at("PART GUITAR").cues.size() == 1u && s.at("PART GUITAR").cues[0].text == "[map HandMap_Solo]");
        CHECK(s.at("PART GUITAR").marks.size() == 1u && s.at("PART GUITAR").marks[0].pitch == 45u &&
              s.at("PART GUITAR").marks[0].length == 480u);
        CHECK(s.at("PART DRUMS").marks.size() == 1u && s.at("PART DRUMS").marks[0].pitch == 24u);
        CHECK(s.at("HARM1").cues.size() == 1u && s.at("HARM1").marks.empty());
        CHECK(s.at("VENUE").marks.size() == 1u && s.at("VENUE").marks[0].pitch == 110u &&
              s.at("VENUE").cues[0].text == "[lighting (verse)]");
        CHECK(s.at("EVENTS").cues[0].text == "[crowd_clap]" && s.at("EVENTS").marks[0].length == 30u);

        keys.events.push_back(note(480u, 96u, true));
        keys.events.push_back(note(540u, 96u, false));
        file.tracks = {tempo, guitar, keys};
        const auto keyed = parseMid(midi::write(file), {}, why);
        CHECK(keyed && keyed->keys == std::vector<uint32_t>({480u}));
    }

    std::vector<std::pair<uint32_t, std::string>> textsOf(const midi::Track &track);

    // Notes on a track by pitch: each note-on's tick.
    std::vector<uint32_t> notesOf(const midi::Track &track, uint8_t from, uint8_t to)
    {
        std::vector<uint32_t> out;
        for (const midi::Event &e : track.events)
            if (midi::kind(e) == midi::kNoteOn && e.data.size() >= 2u && e.data[1] > 0u && e.data[0] >= from &&
                e.data[0] <= to)
                out.push_back(e.tick);
        return out;
    }

    // The band playing as the chart says: its members' texts as GH2's,
    // the hand's places and maps, else made; the drummer's beat by its
    // snares; the bassist's strumming hand.
    void bandPerformance()
    {
        Chart chart = chart480();
        Difficulty lead;
        for (uint32_t t = 0; t < 32u * 480u; t += 240u)
            lead.gems.push_back({t, static_cast<uint8_t>(1u << (t / 240u % 5u))});
        chart.sections = {{0u, "Verse 1"}, {16u * 480u, "Chorus 1"}};
        // Bars 0 to 3 the backbeat, 4 and 5 half time, 6 every beat (one
        // bar, not kept), 7 the backbeat again.
        for (uint32_t bar = 0; bar < 8u; ++bar)
            for (const uint32_t beat : bar == 4u || bar == 5u ? std::vector<uint32_t>{2u}
                                       : bar == 6u            ? std::vector<uint32_t>{0u, 1u, 2u, 3u}
                                                              : std::vector<uint32_t>{1u, 3u})
            {
                chart.snares.push_back(bar * 1920u + beat * 480u);
                chart.drums.push_back(bar * 1920u + beat * 480u);
            }
        chart.kicks = {0u};
        chart.drums.push_back(0u);
        std::sort(chart.drums.begin(), chart.drums.end());

        {
            midi::Track guitar, events;
            const auto band = addStage({chart, lead, kSameTicks, 32u * 480u}, guitar, events);
            const auto texts = textsOf(guitar);
            const auto hasText = [&](uint32_t tick, const std::string &text)
            { return std::find(texts.begin(), texts.end(), std::pair{tick, text}) != texts.end(); };
            CHECK(hasText(16u * 480u, "[wail_on]") && hasText(32u * 480u, "[wail_off]"));
            CHECK(!notesOf(guitar, 40u, 59u).empty());
            const std::vector<std::pair<uint32_t, std::string>> beats = {
                {0u, "[allbeat]"}, {4u * 1920u, "[half_time]"}, {7u * 1920u, "[allbeat]"}};
            std::vector<std::pair<uint32_t, std::string>> drummed;
            for (const auto &t : textsOf(band[0]))
                if (t.second != "[play]" && t.second != "[idle]")
                    drummed.push_back(t);
            CHECK(drummed == beats);
        }

        Chart::StageTrack own;
        own.cues = {{0u, "[mellow]"},
                    {0u, "[map HandMap_Chord_A]"},
                    {0u, "[map StrumMap_Pick]"},
                    {960u, "[wail_on]"},
                    {1920u, "[wail_off]"},
                    {12000u, "[idle_realtime]"}};
        own.marks = {{0u, 480u, 45u}};
        chart.stage["PART GUITAR"] = own;
        Difficulty bassLead;
        bassLead.gems = {{0u, 1u << kOpen}, {480u, 4u}};
        chart.parts[Part::Bass][3] = bassLead;
        chart.stage["PART BASS"].cues = {{0u, "[map StrumMap_SlapBass]"}, {0u, "[intense]"}};
        midi::Track guitar, events;
        const auto band = addStage({chart, lead, kSameTicks, 32u * 480u}, guitar, events);
        const std::vector<std::pair<uint32_t, std::string>> said = {
            {0u, "[play]"}, {960u, "[wail_on]"}, {1920u, "[wail_off]"}, {12000u, "[idle]"}, {0u, "[map HandMap_Default]"}};
        CHECK(textsOf(guitar) == said);
        CHECK(notesOf(guitar, 40u, 59u) == std::vector<uint32_t>({0u}));
        CHECK(band.size() == 6u && band.back().name == "PART BASS");
        if (band.size() == 6u)
        {
            CHECK(notesOf(band.back(), 96u, 100u) == std::vector<uint32_t>({0u, 480u}));
            const auto bassTexts = textsOf(band.back());
            CHECK(bassTexts.size() == 1u && bassTexts[0].second == "[map StrumMap_SlapBass]");
            const auto played = textsOf(band[1]);
            CHECK(!played.empty() && played.front().first == 0u && played.front().second == "[play]");
        }
    }

    // The made fret hand moves only where a phrase starts: green and red
    // traded, then green over and over, keep it at the third fret; past a
    // two-beat rest, a phrase mostly on orange moves it to the twelfth.
    void handPlaces()
    {
        Chart chart = chart480();
        Difficulty lead;
        for (uint32_t t = 0; t < 16u * 480u; t += 240u)
            lead.gems.push_back({t, static_cast<uint8_t>(t < 8u * 480u ? (t / 240u % 2u ? 2u : 1u) : 1u)});
        for (uint32_t t = 18u * 480u; t < 24u * 480u; t += 240u)
            lead.gems.push_back({t, static_cast<uint8_t>(t % 960u ? 0x10u : 0x18u)});
        midi::Track guitar, events;
        addStage({chart, lead, kSameTicks, 24u * 480u}, guitar, events);
        std::vector<std::pair<uint32_t, uint8_t>> places;
        for (const midi::Event &e : guitar.events)
            if (midi::kind(e) == midi::kNoteOn && e.data[1] > 0u && e.data[0] >= 40u && e.data[0] <= 59u)
                places.push_back({e.tick, e.data[0]});
        const std::vector<std::pair<uint32_t, uint8_t>> want = {{0u, 42u}, {18u * 480u, 51u}};
        CHECK(places == want);
    }

    // A chart too fast for the band played at half: the band and crowd at
    // half from its start, kept through a dip to 200, normal again at 150;
    // the lighting's keyframe each band bar, two of the chart's.
    void bandTempo()
    {
        Chart chart;
        chart.resolution = 480u;
        chart.tempos = {{0u, 266667u}, {16u * 480u, 300000u}, {24u * 480u, 400000u}};
        Difficulty lead;
        for (uint32_t t = 0; t < 40u * 480u; t += 240u)
            lead.gems.push_back({t, 1u});
        midi::Track guitar, events;
        const auto band = addStage({chart, lead, kSameTicks, 40u * 480u}, guitar, events);
        const auto only = [](const std::vector<std::pair<uint32_t, std::string>> &texts, const std::string &part)
        {
            std::vector<std::pair<uint32_t, std::string>> out;
            for (const auto &t : texts)
                if (t.second.find(part) != std::string::npos)
                    out.push_back(t);
            return out;
        };
        const std::vector<std::pair<uint32_t, std::string>> played = {{0u, "[half_tempo]"}, {24u * 480u, "[normal_tempo]"}};
        CHECK(only(textsOf(guitar), "_tempo") == played);
        CHECK(band.size() >= 4u && only(textsOf(band[0]), "_tempo") == played && only(textsOf(band[1]), "_tempo") == played);
        const std::vector<std::pair<uint32_t, std::string>> crowd = {{0u, "[crowd_half_tempo]"},
                                                                     {24u * 480u, "[crowd_normal_tempo]"}};
        CHECK(only(textsOf(events), "crowd_") == crowd);
        // Keyframes at bars 0, 2, 4 (the half), then 6, 7, 8, 9.
        CHECK(band.size() >= 4u &&
              notesOf(band[3], 48u, 48u) == std::vector<uint32_t>({0u, 3840u, 7680u, 11520u, 13440u, 15360u, 17280u}));
    }

    // A chart made for GH2, everything its own: its BAND tracks and
    // TRIGGERS as written, its drummer's beat, lighting and tempo, none
    // made over them; GH1's drum cue a beat ahead of its drummer's.
    void authoredStage()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo, guitar, drums, triggers, events;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0x03u, 0xd0u, 0x90u}}); // 240 BPM
        guitar.name = "PART GUITAR";
        for (uint32_t t = 0; t < 16u * 480u; t += 240u)
            guitar.events.push_back(note(t, 96u, true)), guitar.events.push_back(note(t + 60u, 96u, false));
        drums.name = "BAND DRUMS";
        drums.events = {midi::text(960u, "[play]"), midi::text(960u, "[half_time]"), note(960u, 36u, true),
                        note(1000u, 36u, false), midi::text(4800u, "[idle]")};
        triggers.name = "TRIGGERS";
        triggers.events = {note(0u, 48u, true), note(10u, 48u, false)};
        events.name = "EVENTS";
        events.events = {midi::text(0u, "[lighting (strobe)]"), midi::text(0u, "[chorus]"),
                         midi::text(0u, "[crowd_half_tempo]")};
        file.tracks = {tempo, guitar, drums, triggers, events};
        std::string why;
        const auto chart = parseMid(midi::write(file), {}, why);
        CHECK(chart && chart->stage.count("BAND DRUMS") && chart->stage.count("TRIGGERS"));
        if (!chart)
            return;
        const Difficulty &lead = chart->parts.at(Part::Guitar)[3];
        midi::Track out, outEvents;
        const auto band = addStage({*chart, lead, kSameTicks, 16u * 480u}, out, outEvents);
        const auto named = [&](const std::string &name)
        { return std::find_if(band.begin(), band.end(), [&](const midi::Track &t) { return t.name == name; }); };
        const auto ownDrums = named("BAND DRUMS"), ownTriggers = named("TRIGGERS"), upcoming = named("GH1 UPCOMING");
        CHECK(ownDrums != band.end() && ownTriggers != band.end() && upcoming != band.end());
        if (ownDrums == band.end() || ownTriggers == band.end() || upcoming == band.end())
            return;
        const std::vector<std::pair<uint32_t, std::string>> drummer = {
            {960u, "[play]"}, {960u, "[half_time]"}, {4800u, "[idle]"}};
        CHECK(textsOf(*ownDrums) == drummer);
        CHECK(notesOf(*ownDrums, 36u, 36u) == std::vector<uint32_t>({960u}));
        // Its own lighting's keyframes alone.
        CHECK(notesOf(*ownTriggers, 0u, 127u) == std::vector<uint32_t>({0u}));
        CHECK(notesOf(*upcoming, 60u, 60u) == std::vector<uint32_t>({480u}) &&
              notesOf(*upcoming, 61u, 61u) == std::vector<uint32_t>({4320u}));
        // Its lighting and look alone, and no tempo made at 240 BPM.
        const std::vector<std::pair<uint32_t, std::string>> lit = {
            {0u, "[lighting (strobe)]"}, {0u, "[chorus]"}, {0u, "[crowd_half_tempo]"}};
        CHECK(textsOf(outEvents) == lit);
        CHECK(std::none_of(out.events.begin(), out.events.end(), [](const midi::Event &e)
                           { return std::string(e.data.begin(), e.data.end()).find("_tempo]") != std::string::npos; }));
    }

    // A chart made for GH2 with only some of the stage: what it writes as
    // written, the rest made around it. Its drummer plays from bar 2 to 4
    // and nothing says where it kicks, so it kicks there alone; its look
    // but no lighting, so lighting and keyframes made, no look; lighters
    // kept.
    void partlyAuthored()
    {
        midi::File file{1u, 480u, {}};
        midi::Track tempo, guitar, drums, events;
        tempo.events.push_back({0u, midi::kMeta, midi::kTempo, {0x07u, 0xa1u, 0x20u}}); // 120 BPM
        guitar.name = "PART GUITAR";
        for (uint32_t t = 0; t < 32u * 480u; t += 240u)
            guitar.events.push_back(note(t, 96u, true)), guitar.events.push_back(note(t + 60u, 96u, false));
        drums.name = "BAND DRUMS";
        drums.events = {midi::text(1920u, "[play]"), midi::text(3840u, "[idle]")};
        events.name = "EVENTS";
        events.events = {midi::text(0u, "[verse]"), midi::text(1920u, "[crowd_lighters_slow]")};
        file.tracks = {tempo, guitar, drums, events};
        std::string why;
        const auto chart = parseMid(midi::write(file), {}, why);
        CHECK(chart.has_value());
        if (!chart)
            return;
        const Difficulty &lead = chart->parts.at(Part::Guitar)[3];
        midi::Track out, outEvents;
        const auto band = addStage({*chart, lead, kSameTicks, 32u * 480u}, out, outEvents);
        const auto named = [&](const std::string &name)
        { return std::find_if(band.begin(), band.end(), [&](const midi::Track &t) { return t.name == name; }); };
        const auto ownDrums = named("BAND DRUMS"), triggers = named("TRIGGERS");
        CHECK(ownDrums != band.end() && triggers != band.end());
        if (ownDrums == band.end() || triggers == band.end())
            return;
        const auto kicks = notesOf(*ownDrums, 36u, 36u);
        CHECK(!kicks.empty() && kicks.front() >= 1920u && kicks.back() <= 3840u);
        const auto states = textsOf(*ownDrums);
        CHECK(std::count_if(states.begin(), states.end(), [](const auto &s) { return s.second == "[play]"; }) == 1);
        CHECK(!notesOf(*triggers, 48u, 48u).empty());
        const auto texts = textsOf(outEvents);
        const auto count = [&](const auto &kind)
        { return std::count_if(texts.begin(), texts.end(), [&](const auto &s) { return kind(s.second); }); };
        CHECK(count([](const std::string &t) { return t == "[verse]" || t == "[chorus]" || t == "[solo]"; }) == 1);
        CHECK(count([](const std::string &t) { return t.rfind("[lighting", 0) == 0; }) > 0);
        CHECK(count([](const std::string &t) { return t == "[crowd_lighters_slow]"; }) == 1);
    }

    // A track's texts at each tick, in order.
    std::vector<std::pair<uint32_t, std::string>> textsOf(const midi::Track &track)
    {
        std::vector<std::pair<uint32_t, std::string>> out;
        for (const midi::Event &e : track.events)
            if (e.status == midi::kMeta && e.meta == midi::kText)
                out.emplace_back(e.tick, std::string(e.data.begin(), e.data.end()));
        return out;
    }

    // The band made from a chart: names less their numbers, a look by
    // name or by how busy, the drummer on the beat with no drums, the
    // singer only where there is singing, a bar's keyframe.
    void stage()
    {
        CHECK(sectionBaseName("Verse 1A") == "verse" && sectionBaseName("chorus_2b") == "chorus" &&
              sectionBaseName("Gtr Hook") == "gtr hook");
        CHECK(sectionBaseName("verse1a") == "verse" && sectionBaseName("Solo") == "solo" && sectionBaseName("Outro") == "outro");

        Chart chart = chart480();
        Difficulty lead;
        // Sparse in "Riff" and "Riff 2", busy in "Big Part": no name says.
        for (uint32_t t = 0; t < 32u * 480u; t += 960u)
            lead.gems.push_back({t, 1u});
        for (uint32_t t = 32u * 480u; t < 48u * 480u; t += 120u)
            lead.gems.push_back({t, 2u});
        for (uint32_t t = 48u * 480u; t < 64u * 480u; t += 960u)
            lead.gems.push_back({t, 1u});
        chart.sections = {{0u, "Riff 1"}, {32u * 480u, "Big Part"}, {48u * 480u, "Riff 2"}};
        chart.vocals = {{480u * 4u, 480u * 8u}};
        midi::Track guitar, events;
        const auto band = addStage({chart, lead, kSameTicks, 64u * 480u}, guitar, events);
        CHECK(band.size() == 5u && band[4].name == "GH1 UPCOMING");
        if (band.size() != 5u)
            return;
        const auto ev = textsOf(events);
        const auto has = [&](uint32_t tick, const std::string &text)
        { return std::find(ev.begin(), ev.end(), std::pair{tick, text}) != ev.end(); };
        CHECK(has(0u, "[verse]") && has(32u * 480u, "[chorus]") && has(48u * 480u, "[verse]"));
        // The same look for both riffs.
        std::string first, second;
        for (const auto &[tick, text] : ev)
            if (text.rfind("[lighting", 0) == 0 && tick == 0u)
                first = text;
            else if (text.rfind("[lighting", 0) == 0 && tick == 48u * 480u)
                second = text;
        CHECK(!first.empty() && first == second);
        const auto singer = textsOf(band[2]);
        const std::vector<std::pair<uint32_t, std::string>> sung = {{1920u, "[play]"}, {5760u, "[idle]"}};
        CHECK(singer == sung);
        // No drums: a kick each beat while the band plays.
        int kicks = 0, keys = 0;
        for (const midi::Event &e : band[0].events)
            kicks += midi::kind(e) == midi::kNoteOn && e.data[0] == 36u;
        for (const midi::Event &e : band[3].events)
            keys += midi::kind(e) == midi::kNoteOn && e.data[0] == 48u;
        CHECK(kicks == 63); // beats 0 to 62, the last note's

        CHECK(keys == 16);
        CHECK(!textsOf(guitar).empty() && textsOf(guitar).front().second == "[play]");

        // A keys part: the keyboard player's track, playing where it does.
        chart.keys = {480u * 8u, 480u * 9u};
        midi::Track guitar2, events2;
        const auto keyed = addStage({chart, lead, kSameTicks, 64u * 480u}, guitar2, events2);
        CHECK(keyed.size() == 6u && keyed[4].name == "BAND KEYS");
        if (keyed.size() == 6u)
        {
            const std::vector<std::pair<uint32_t, std::string>> played = {{3600u, "[play]"}, {4800u, "[idle]"}};
            CHECK(textsOf(keyed[4]) == played);
        }
    }

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
            CHECK(back && back->tracks.size() > 1u);
            if (back && back->tracks.size() > 1u)
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
            CHECK(back && back->tracks.size() > 1u);
            if (back && back->tracks.size() > 1u)
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
        CHECK(at(x, 0) && at(x, 0)->inLanePhrase && at(x, 0)->lanePhraseStart && at(x, 0)->longest() == 0u);
        CHECK(at(x, 192) && at(x, 192)->inLanePhrase && !at(x, 192)->lanePhraseStart && at(x, 192)->longest() == 50u);
        CHECK(at(x, 288) && !at(x, 288)->inLanePhrase);
        CHECK(at(x, 768) && at(x, 768)->lanePhraseStart && at(x, 1056) && at(x, 1056)->inLanePhrase);
        CHECK(at(x, 1152) && !at(x, 1152)->inLanePhrase && at(x, 1152)->longest() == 300u);
        CHECK(at(x, 1536) && !at(x, 1536)->inLanePhrase);
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
        CHECK(at(x, 3072) && at(x, 3072)->lanePhraseStart && at(x, 3360) && at(x, 3360)->inLanePhrase);

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
            CHECK(at(d[3], 240) && at(d[3], 240)->inLanePhrase);
            CHECK(at(d[2], 240) && at(d[2], 240)->inLanePhrase == (velocity == 45u));
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

    // Who stands at the front: the singer wherever anything says there is
    // singing, a keyboard player in place of none, no one only for a split
    // multitrack with drums and bass charted and nothing sung.
    void bandFronts()
    {
        const auto front = [](uint8_t band, uint8_t parts, std::vector<std::string> stems, const char *gender = "")
        {
            Song s = song("/c/a", "A");
            s.chartHas = band;
            s.parts = parts;
            s.stems = std::move(stems);
            if (*gender)
                s.tags["vocal_gender"] = gender;
            const char *f = bandFront(s);
            return f ? std::string("=") + f : std::string("default");
        };
        const uint8_t guitar = 1u << int(Part::Guitar), both = guitar | 1u << int(Part::Bass);
        CHECK(front(0u, guitar, {"song.ogg"}) == "default");
        CHECK(front(kHasVocals, guitar, {"song.ogg"}, "Female") == "=female_singer");
        CHECK(front(0u, guitar, {"song.ogg"}, "female") == "=female_singer");
        CHECK(front(kHasKeys, guitar, {"song.ogg"}) == "=metal_keyboard");
        CHECK(front(kHasKeys | kHasVocals, guitar, {"song.ogg"}) == "default");
        CHECK(front(kHasKeys, guitar, {"song.ogg", "vocals_1.ogg"}) == "default");
        CHECK(front(kHasDrums, both, {"song.ogg", "drums_1.ogg", "bass.ogg"}) == "=");
        CHECK(front(kHasDrums, both, {"song.ogg", "bass.ogg"}) == "default");
        CHECK(front(kHasDrums, guitar, {"song.ogg", "drums.ogg"}) == "default");
        CHECK(front(kHasDrums, both, {"song.ogg", "drums.ogg", "vocals.ogg"}) == "default");

        const fs::path file = fs::temp_directory_path() / "ghc_index_band_test.txt";
        Song keyed = song("/c/a/keys", "Keys");
        keyed.chartHas = kHasKeys | kHasDrums;
        keyed.bandBpm = 112.5f;
        writeIndex(file, {keyed});
        const std::vector<Song> back = readIndex(file);
        CHECK(back.size() == 1u && back[0].chartHas == (kHasKeys | kHasDrums) && tag(back[0].tags, "name") == "Keys" &&
              back[0].bandBpm == 112.5f);
        fs::remove(file);
    }

    // Names keyed as tools/song_facts.py's key() keys them (its output for
    // each, copied here), and the official songs' bands from the table:
    // the song's own, else its artist's gender; song.ini's vocal_gender
    // over either.
    void songFactsTable()
    {
        const std::pair<const char *, const char *> keys[] = {
            {"The Edgar Winter Group", "edgarwintergroup"}, {"M\xc3\xb6tley Cr\xc3\xbc" "e", "motleycrue"},
            {"Simon & Garfunkel", "simonandgarfunkel"},     {"Song (Live) feat. Someone", "song"},
            {"AC/DC", "acdc"},                              {"  Th\xc3\xa9  Who ", "who"},
            {"Beyonc\xc3\xa9 ft Jay-Z", "beyonce"},         {"[Bonus] Track (x", "trackx"},
            {"Theory of a Deadman", "theoryofadeadman"},    {"Caf\xc3\xa9 del Mar featuring X", "cafedelmar"},
        };
        for (const auto &[name, want] : keys)
        {
            CHECK(factKey(name) == want);
            if (factKey(name) != want)
                std::cerr << "  " << name << " -> " << factKey(name) << ", not " << want << "\n";
        }

        const auto named = [](const char *artist, const char *title, uint8_t band = 0u)
        {
            Song s = song("/c/a", title);
            s.tags["artist"] = artist;
            s.chartHas = band;
            s.parts = 1u << int(Part::Guitar);
            s.stems = {"song.ogg"};
            const char *f = bandFront(s);
            return f ? std::string("=") + f : std::string("default");
        };
        // A game's own: Jessica's keyboard player, Soy Bomb's no one,
        // Misery Business's singer female; Freezepop by its artist.
        CHECK(named("The Allman Brothers Band", "Jessica") == "=metal_keyboard");
        CHECK(named("Honest Bob and the Factory-to-Dealer Incentives", "Soy Bomb") == "=");
        CHECK(named("Paramore", "Misery Business") == "=female_singer");
        CHECK(named("Freezepop", "I Am Not Your Gameboy") == "=female_singer");
        CHECK(named("Rush", "Tom Sawyer", kHasKeys) == "default"); // a singer over a keys part
        CHECK(named("Nobody Known", "Nothing") == "default");
        Song told = song("/c/a", "Misery Business");
        told.tags["artist"] = "Paramore";
        told.tags["vocal_gender"] = "male";
        CHECK(bandFront(told) == nullptr);

        // The band's clips: the game's, else from 120 BPM.
        Song fast = song("/c/a", "Jessica"), slow = song("/c/a", "Gemini"), other = song("/c/a", "Nothing");
        fast.tags["artist"] = "The Allman Brothers Band";
        slow.tags["artist"] = "Brian Kahanek";
        other.tags["artist"] = "Nobody Known";
        CHECK(bandFast(fast) && !bandFast(slow));
        other.bandBpm = 119.0f;
        CHECK(!bandFast(other));
        other.bandBpm = 150.0f;
        CHECK(bandFast(other));
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
    bandParts();
    stageTracks();
    syllables();
    lipSync();
    drumPieces();
    bandFronts();
    songFactsTable();
    bandPerformance();
    bandTempo();
    authoredStage();
    partlyAuthored();
    handPlaces();
    stage();
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
