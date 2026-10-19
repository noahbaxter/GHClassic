// notes.mid is Rock Band's form as Clone Hero reads it: each difficulty's
// lanes from note 60, 72, 84 and 96 (easy to expert), the next two its
// forced HOPO and forced strum, the one before its open where the track
// says [ENHANCED_OPENS]; 104 taps, 116 star power, 103 a solo (or, where
// nothing is on 116, star power: each difficulty's on the note past its
// frets, 67 to 103, as GH1 and GH2's, where those are); 105 and 106 each
// player's Face-Off phrases (or each difficulty's two past its star power,
// 69 and 70 to 105 and 106, as GH2's), 110 GH2's big notes, 120 a big rock
// ending, 126 and 127 tremolo and trill phrases (Expert's, Hard's too at
// velocity 41 to 50). Phase Shift's sysex, F0 'P' 'S' 0 0
// <difficulty, FF all> <1 open, 4 tap> <1 on, 0 off> F7, marks opens and
// taps too.
//
// Past the five-fret parts, each read only for the band to move by: the
// drums' expert hits (PART DRUMS, [ExpertDrums]) and when there is
// singing (PART VOCALS, or [Events]' phrase_start, phrase_end and lyric).

#include "custom/chart.h"

#include "custom/chart_rules.h"

#include "custom/text.h"
#include "formats/midi.h"
#include "settings/ini.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace gh2::custom
{
    using namespace rules;

    namespace
    {
        constexpr uint8_t kTremoloNote = 126u, kTrillNote = 127u;
        constexpr int kOpenPhrase = 0, kTapPhrase = 1; // Phase Shift's sysex phrases
        constexpr size_t kAllDifficulties = 4u;        // a sysex phrase's difficulty FF

        // [on, off) spans of one note number.
        using Spans = std::vector<Span>;

        bool within(const Spans &spans, uint32_t tick)
        {
            for (const Span &s : spans)
                if (tick >= s.tick && tick < s.tick + std::max(s.length, 1u))
                    return true;
            return false;
        }

        const std::map<std::string, Part> kMidParts = {
            {"PART GUITAR", Part::Guitar}, {"T1 GEMS", Part::Guitar}, {"PART GUITAR COOP", Part::Coop},
            {"PART RHYTHM", Part::Rhythm}, {"PART BASS", Part::Bass},
        };

        std::array<Difficulty, 4> midPart(const midi::Track &track, uint32_t resolution,
                                          std::optional<uint32_t> threshold, const Options &options)
        {
            std::map<uint8_t, Spans> notes;
            std::map<uint8_t, uint32_t> held;
            std::map<uint8_t, uint32_t> endedAt; // where a note struck again ended the held one
            // Lane phrases Hard has too: struck at velocity 41 to 50 (YARG's
            // MidReader), by note and tick.
            std::set<std::pair<uint8_t, uint32_t>> hardLanePhrases;
            // Sysex phrases by kind and difficulty.
            std::array<std::array<Spans, kAllDifficulties + 1u>, 2> phrases;
            std::array<std::array<std::optional<uint32_t>, kAllDifficulties + 1u>, 2> phraseOn;
            bool enhancedOpens = false;
            for (const midi::Event &e : track.events)
            {
                if (e.status == midi::kMeta)
                {
                    const std::string text(e.data.begin(), e.data.end());
                    if (e.meta == midi::kText && (text == "[ENHANCED_OPENS]" || text == "ENHANCED_OPENS"))
                        enhancedOpens = true;
                    continue;
                }
                if (e.status == midi::kSysex)
                {
                    const midi::Bytes &bytes = e.data;
                    if (bytes.size() < 7u || bytes[0] != 'P' || bytes[1] != 'S' || bytes[2] != 0u || bytes[3] != 0u)
                        continue;
                    const int phraseKind = bytes[5] == 1u ? kOpenPhrase : bytes[5] == 4u ? kTapPhrase : -1;
                    if (phraseKind < 0 || (bytes[4] > 3u && bytes[4] != 0xffu))
                        continue;
                    const size_t difficulty = bytes[4] == 0xffu ? kAllDifficulties : bytes[4];
                    auto &on = phraseOn[phraseKind][difficulty];
                    if (bytes[6] != 0u)
                        on = e.tick;
                    else if (on)
                    {
                        phrases[phraseKind][difficulty].push_back({*on, e.tick - *on});
                        on.reset();
                    }
                    continue;
                }
                const uint8_t type = midi::kind(e);
                if ((type != midi::kNoteOn && type != midi::kNoteOff) || e.data.size() < 2u)
                    continue;
                const uint8_t pitch = e.data[0];
                const bool down = type == midi::kNoteOn && e.data[1] != 0u;
                const auto it = held.find(pitch);
                // A note struck again while held ends the first there; that
                // note's own end, written after on the same tick, is the
                // first's and not the new one's.
                if (const auto ended = endedAt.find(pitch);
                    !down && it != held.end() && it->second == e.tick && ended != endedAt.end() && ended->second == e.tick)
                {
                    endedAt.erase(ended);
                    continue;
                }
                if (it != held.end())
                {
                    notes[pitch].push_back({it->second, e.tick - it->second});
                    held.erase(it);
                    if (down)
                        endedAt[pitch] = e.tick;
                }
                if (down)
                {
                    held[pitch] = e.tick;
                    if ((pitch == kTremoloNote || pitch == kTrillNote) && e.data[1] >= 41u && e.data[1] <= 50u)
                        hardLanePhrases.insert({pitch, e.tick});
                }
            }
            for (const auto &[pitch, on] : held)
                notes[pitch].push_back({on, 0u});

            // Star power: Rock Band's 116 for all four, else GH1 and GH2's
            // note past each difficulty's frets (67, 79, 91, 103) where
            // those are, else 103 for all four.
            const bool rb = !options.starPowerOn103 && !notes[116].empty();
            const bool spPerDifficulty = !(notes[67].empty() && notes[79].empty() && notes[91].empty());
            // Face-Off phrases, GH2's a difficulty (69 and 70 to 105 and
            // 106), else Rock Band's 105 and 106 for all four.
            const bool faceOffPerDifficulty = !(notes[69].empty() && notes[70].empty() && notes[81].empty() &&
                                                notes[82].empty() && notes[93].empty() && notes[94].empty());
            const Spans &allOpens = phrases[kOpenPhrase][kAllDifficulties], &allTaps = phrases[kTapPhrase][kAllDifficulties];
            std::array<Difficulty, 4> out;
            for (size_t d = 0; d < 4u; ++d)
            {
                const uint8_t base = static_cast<uint8_t>(60u + 12u * d);
                std::map<uint32_t, Raw> rawsByTick;
                const auto addFret = [&](uint8_t pitch, int fret)
                {
                    for (const Span &s : notes[pitch])
                    {
                        Raw &r = rawsByTick[s.tick];
                        r.gem.tick = s.tick;
                        r.add(fret, s.length);
                    }
                };
                for (int fret = 0; fret < 5; ++fret)
                    addFret(static_cast<uint8_t>(base + fret), fret);
                if (enhancedOpens)
                    addFret(static_cast<uint8_t>(base - 1u), kOpen);
                Spans opens = phrases[kOpenPhrase][d], taps = phrases[kTapPhrase][d];
                opens.insert(opens.end(), allOpens.begin(), allOpens.end());
                taps.insert(taps.end(), allTaps.begin(), allTaps.end());
                taps.insert(taps.end(), notes[104].begin(), notes[104].end());
                for (auto &[tick, r] : rawsByTick)
                {
                    // Phase Shift's open is the green lane's note.
                    if (within(opens, tick) && r.gem.lanes == 1u)
                    {
                        r.gem.lanes = 1u << kOpen;
                        r.gem.length[kOpen] = r.gem.length[0];
                        r.gem.length[0] = 0u;
                    }
                    r.hopo = within(notes[base + 5u], tick);
                    r.strum = within(notes[base + 6u], tick);
                    r.tap = within(taps, tick);
                }
                // Lane phrases on Expert, and Hard where it has them too.
                for (const auto &[pitch, into] : {std::pair{kTremoloNote, &out[d].tremolos}, std::pair{kTrillNote, &out[d].trills}})
                    for (const Span &s : notes[pitch])
                        if (d == 3u || (d == 2u && hardLanePhrases.count({pitch, s.tick})))
                            into->push_back(s);
                settleGems(out[d], rawsByTick, resolution, threshold);
                out[d].starPower = notes[rb ? 116u : spPerDifficulty ? base + 7u : 103u];
                if (rb)
                    out[d].solos = notes[103];
                for (uint8_t player = 0; player < 2u; ++player)
                    out[d].faceOff[player] = notes[(faceOffPerDifficulty ? base + 9u : 105u) + player];
                out[d].bigNotes = notes[110];
                out[d].rockEndings = notes[120];
            }
            return out;
        }

        bool noteOn(const midi::Event &e) { return midi::kind(e) == midi::kNoteOn && e.data.size() >= 2u && e.data[1] > 0u; }
        bool noteOff(const midi::Event &e)
        {
            return (midi::kind(e) == midi::kNoteOff ||
                    (midi::kind(e) == midi::kNoteOn && e.data.size() >= 2u && e.data[1] == 0u)) &&
                   !e.data.empty();
        }

        // PART DRUMS' expert: 96 the kick (95 the second pedal's), 97 to
        // 100 the pads.
        void midDrums(const midi::Track &track, Chart &chart)
        {
            for (const midi::Event &e : track.events)
                if (noteOn(e) && e.data[0] >= 95u && e.data[0] <= 100u)
                {
                    if (e.data[0] <= 96u)
                        chart.kicks.push_back(e.tick);
                    chart.drums.push_back(e.tick);
                }
        }

        // PART VOCALS: its phrases (105, 106) where it marks them, else its
        // notes' runs (36 to 84), else its lyrics', two beats apart.
        void midVocals(const midi::Track &track, Chart &chart)
        {
            std::map<uint8_t, uint32_t> on;
            std::vector<uint32_t> notes, lyrics;
            for (const midi::Event &e : track.events)
            {
                if (e.status == midi::kMeta && e.meta == midi::kLyric)
                    lyrics.push_back(e.tick);
                else if (noteOn(e))
                {
                    if (e.data[0] == 105u || e.data[0] == 106u)
                        on[e.data[0]] = e.tick;
                    else if (e.data[0] >= 36u && e.data[0] <= 84u)
                        notes.push_back(e.tick);
                }
                else if (noteOff(e))
                    if (const auto it = on.find(e.data[0]); it != on.end())
                    {
                        chart.vocals.push_back({it->second, e.tick - it->second});
                        on.erase(it);
                    }
            }
            if (chart.vocals.empty())
                chart.vocals = phrasesFrom(notes.empty() ? lyrics : notes, chart.resolution);
        }
    }

    std::optional<Chart> parseMid(const std::vector<uint8_t> &bytes, const Options &options, std::string &why)
    {
        const auto file = midi::parse(bytes);
        if (!file || file->tracks.empty())
        {
            why = "not a MIDI file";
            return std::nullopt;
        }
        if (file->division & 0x8000u)
        {
            why = "timed in SMPTE frames";
            return std::nullopt;
        }
        if (file->division == 0u)
        {
            why = "no ticks a beat";
            return std::nullopt;
        }
        Chart chart;
        chart.resolution = file->division;
        chart.hopoThreshold = songThreshold(options, chart.resolution);
        for (const midi::Event &e : file->tracks[0].events)
            if (e.status == midi::kMeta && e.meta == midi::kTempo && e.data.size() >= 3u)
            {
                // A tempo of 0 is none, as midi::TempoMap has it.
                if (const uint32_t us = uint32_t(e.data[0]) << 16 | uint32_t(e.data[1]) << 8 | e.data[2]; us != 0u)
                    chart.tempos.push_back({e.tick, us});
            }
            else if (e.status == midi::kMeta && e.meta == midi::kSignature && e.data.size() >= 2u && e.data[0] != 0u)
                chart.signatures.push_back({e.tick, {e.data[0], e.data[1]}});
        for (const midi::Track &track : file->tracks)
        {
            if (track.name == "EVENTS")
                for (const midi::Event &e : track.events)
                    if (e.status == midi::kMeta && e.meta == midi::kText)
                        if (auto name = sectionName(std::string(e.data.begin(), e.data.end())))
                            chart.sections.emplace_back(e.tick, *name);
            if (track.name == "PART DRUMS" && chart.drums.empty())
                midDrums(track, chart);
            if (track.name == "PART VOCALS" && chart.vocals.empty())
                midVocals(track, chart);
            const auto part = kMidParts.find(track.name);
            if (part == kMidParts.end() || chart.parts.count(part->second))
                continue;
            auto read = midPart(track, chart.resolution, chart.hopoThreshold, options);
            if (hasGems(read))
                chart.parts[part->second] = std::move(read);
        }
        finish(chart);
        return chart;
    }
}
