// What reading a notes.mid and a notes.chart follow alike
// (custom/chart_rules.h).
//
// Lengths are kept as written: each engine cuts its own short sustains as
// the song loads (gameplay/extended/guitar/gem_kinds.h).

#include "custom/chart_rules.h"

#include "settings/ini.h"

#include <string_view>

namespace gh2::custom::rules
{
    namespace
    {
        // A tick at GH2's 480 a beat, as the converter rounds it.
        uint64_t at480(uint64_t tick, uint32_t resolution)
        {
            return (tick * 480u + resolution / 2u) / resolution;
        }

        // Notes starting within 10 ticks of a gem (at 480) are of it, as
        // GH2's parser makes chords (SongParser::ComputeSlots, 0x246088).
        std::vector<Raw> snapped(const std::map<uint32_t, Raw> &raws, uint32_t resolution)
        {
            std::vector<Raw> out;
            out.reserve(raws.size());
            for (const auto &[tick, r] : raws)
            {
                if (!r.gem.lanes)
                    continue;
                if (out.empty() || at480(r.gem.tick, resolution) - at480(out.back().gem.tick, resolution) > 10u)
                {
                    out.push_back(r);
                    continue;
                }
                Raw &into = out.back();
                into.gem.lanes |= r.gem.lanes;
                for (size_t lane = 0; lane < into.gem.length.size(); ++lane)
                    into.gem.length[lane] = std::max(into.gem.length[lane], r.gem.length[lane]);
                into.hopo |= r.hopo, into.strum |= r.strum, into.flip |= r.flip, into.tap |= r.tap;
            }
            return out;
        }

        // Each gem's kind, forced or natural. Natural is GH2's (GameGemList::
        // WillBeNoStrum, 0x24d360): a single note on no fret of the gem
        // before, under the threshold after it. Only an open's natural kind
        // is final here; for the rest the engine judging decides it again as
        // the song loads.
        std::vector<Gem> settle(const std::map<uint32_t, Raw> &read, uint32_t resolution, std::optional<uint32_t> threshold)
        {
            const uint32_t hopoTicks = threshold.value_or(kGh2HopoThreshold);
            std::vector<Gem> out;
            std::vector<Raw> raws = snapped(read, resolution);
            out.reserve(raws.size());
            for (Raw &r : raws)
            {
                Gem &g = r.gem;
                const Gem *prev = out.empty() ? nullptr : &out.back();
                const bool natural = prev && !g.chord() && (g.lanes & prev->lanes) == 0u &&
                                     at480(g.tick, resolution) - at480(prev->tick, resolution) < hopoTicks;
                if (r.tap)
                    g.kind = Kind::Tap;
                else if (r.hopo)
                    g.kind = Kind::Hopo;
                else if (r.strum)
                    g.kind = Kind::Strum;
                else
                    g.kind = natural != r.flip ? Kind::Hopo : Kind::Strum;
                g.forced = r.tap || r.hopo || r.strum;
                g.flip = r.flip && !g.forced;
                out.push_back(g);
            }
            return out;
        }

        // A lane phrase's notes, as YARG picks them (MoonSongLoader.Guitar):
        // a tremolo one mask struck at least twice, cut where it changes; a
        // trill two single notes alternating, at least three, cut where
        // they stop.
        std::vector<Gem *> tremolo(const std::vector<Gem *> &notes)
        {
            if (notes.size() < 2u || notes[0]->lanes != notes[1]->lanes)
                return {};
            std::vector<Gem *> out;
            for (Gem *g : notes)
            {
                if (g->lanes != notes[0]->lanes)
                    break;
                out.push_back(g);
            }
            return out;
        }
        std::vector<Gem *> trill(const std::vector<Gem *> &notes)
        {
            if (notes.size() < 3u || notes[0]->chord() || notes[1]->chord() || notes[0]->lanes == notes[1]->lanes ||
                notes[2]->lanes != notes[0]->lanes)
                return {};
            std::vector<Gem *> out;
            for (size_t i = 0; i < notes.size() && notes[i]->lanes == notes[i % 2u]->lanes; ++i)
                out.push_back(notes[i]);
            return out;
        }

        // A difficulty's lane phrase notes and bands. A phrase holds the
        // notes from its start to its end, that tick too unless another
        // phrase starts there (MoonSongLoader.GetNotesInLanePhrase). Its
        // notes hold no sustain but its last.
        void markLanePhrases(Difficulty &d)
        {
            std::vector<std::pair<Span, bool>> phrases;
            for (const Span &s : d.tremolos)
                phrases.emplace_back(s, false);
            for (const Span &s : d.trills)
                phrases.emplace_back(s, true);
            std::stable_sort(phrases.begin(), phrases.end(), [](const auto &a, const auto &b) { return a.first.tick < b.first.tick; });
            const auto phraseStartsAt = [&](uint64_t tick)
            {
                return std::any_of(phrases.begin(), phrases.end(), [&](const auto &p) { return p.first.tick == tick; });
            };
            for (const auto &[s, isTrill] : phrases)
            {
                const uint64_t end = uint64_t(s.tick) + s.length;
                std::vector<Gem *> notes;
                for (Gem &g : d.gems)
                    if (g.tick >= s.tick && (g.tick < end || (g.tick == end && !phraseStartsAt(end))) && !g.inLanePhrase)
                        notes.push_back(&g);
                const auto phraseNotes = isTrill ? trill(notes) : tremolo(notes);
                if (phraseNotes.empty())
                    continue;
                phraseNotes.front()->lanePhraseStart = true;
                std::map<int, Span> reach; // each fret's first to last note in the phrase
                for (Gem *g : phraseNotes)
                {
                    g->inLanePhrase = true;
                    if (g != phraseNotes.back())
                        g->length = {};
                    for (int lane = 0; lane <= kOpen; ++lane)
                        if (g->lanes & (1u << lane))
                        {
                            const auto it = reach.try_emplace(lane, Span{g->tick, 0u}).first;
                            it->second.length = g->tick - it->second.tick;
                        }
                }
                for (const auto &[lane, s] : reach)
                    d.bands.push_back({s.tick, s.length, lane});
            }
            sortByTick(d.bands);
        }

        // A lyric as sung: its markup off (- + = # ^ * % $ / and <tags>), §
        // and _ spaces, its spaces trimmed. A '+' alone is a slide, none.
        std::string sung(const std::string &lyric)
        {
            std::string out;
            bool tag = false;
            for (const char c : lyric)
            {
                if (c == '<')
                    tag = true;
                else if (c == '>')
                    tag = false;
                else if (tag || std::string_view("-+=#^*%$/").find(c) != std::string_view::npos)
                    continue;
                else
                    out += c == '_' ? ' ' : c;
            }
            for (size_t at; (at = out.find("\xc2\xa7")) != std::string::npos;)
                out.replace(at, 2u, " ");
            return ini::trim(out);
        }

        // Syllables with no note held to the next, a beat at most.
        void holdSyllables(Chart &chart)
        {
            sortByTick(chart.syllables);
            for (size_t i = 0; i < chart.syllables.size(); ++i)
                if (chart.syllables[i].length == 0u)
                {
                    const uint32_t next = i + 1u < chart.syllables.size() ? chart.syllables[i + 1u].tick : UINT32_MAX;
                    chart.syllables[i].length =
                        static_cast<uint32_t>(std::min<uint64_t>(chart.resolution, uint64_t(next) - chart.syllables[i].tick));
                }
        }
    }

    std::optional<uint32_t> songThreshold(const Options &options, uint32_t resolution)
    {
        uint32_t at = 0u;
        // Capped so a wild song.ini value stays a sane uint32_t.
        if (options.hopoFrequency)
            at = static_cast<uint32_t>(std::min<uint64_t>(at480(*options.hopoFrequency, resolution), 1u << 24));
        else if (options.eighthNoteHopo)
            at = 240u;
        if (at == 0u || at == kGh2HopoThreshold)
            return std::nullopt;
        return at;
    }

    void settleGems(Difficulty &d, const std::map<uint32_t, Raw> &raws, uint32_t resolution,
                    std::optional<uint32_t> threshold)
    {
        d.gems = settle(raws, resolution, threshold);
        markLanePhrases(d);
    }

    bool hasGems(const std::array<Difficulty, 4> &part)
    {
        return std::any_of(part.begin(), part.end(), [](const Difficulty &d) { return !d.gems.empty(); });
    }

    std::optional<std::string> sectionName(const std::string &text)
    {
        std::string s = text;
        if (s.size() >= 2u && s.front() == '[' && s.back() == ']')
            s = s.substr(1u, s.size() - 2u);
        for (const char *prefix : {"section ", "prc_"})
            if (s.rfind(prefix, 0) == 0)
                return ini::trim(s.substr(std::char_traits<char>::length(prefix)));
        return std::nullopt;
    }

    void addDrum(Chart &chart, std::vector<PadHit> &pads, uint32_t tick, int lane)
    {
        if (lane == 0 || lane == 32)
            chart.kicks.push_back(tick);
        else if (lane == 1)
            chart.snares.push_back(tick);
        else
            pads.push_back({tick, lane});
        chart.drums.push_back(tick);
    }

    void cymbals(Chart &chart, const std::vector<PadHit> &pads, bool pro)
    {
        for (const PadHit &hit : pads)
            if (hit.cymbal && (hit.lane == kGreenPad || (pro && hit.lane == kBluePad)))
                chart.crashes.push_back(hit.tick);
    }

    void addLyric(Chart &chart, uint32_t tick, uint32_t length, const std::string &text)
    {
        if (!chart.syllables.empty() && chart.syllables.back().tick == tick)
            return; // the same lyric twice
        const std::string word = sung(text);
        if (word.empty())
        {
            if (!chart.syllables.empty() && text.find('+') != std::string::npos && tick >= chart.syllables.back().tick)
                chart.syllables.back().length = tick + length - chart.syllables.back().tick;
            return;
        }
        chart.syllables.push_back({tick, length, word});
    }

    std::vector<Span> phrasesFrom(const std::vector<uint32_t> &ticks, uint32_t resolution)
    {
        return runs(ticks, resolution * 2u, resolution / 2u);
    }

    void finish(Chart &chart)
    {
        sortByTick(chart.tempos);
        if (chart.tempos.empty() || chart.tempos.front().tick != 0u)
            chart.tempos.insert(chart.tempos.begin(), Tempo{});
        const auto byFirst = [](const auto &a, const auto &b) { return a.first < b.first; };
        std::stable_sort(chart.signatures.begin(), chart.signatures.end(), byFirst);
        std::stable_sort(chart.sections.begin(), chart.sections.end(), byFirst);
        sortByTick(chart.vocals);
        for (auto &[name, track] : chart.stage)
            sortByTick(track.cues), sortByTick(track.marks);
        for (auto &[part, difficulties] : chart.parts)
            for (Difficulty &d : difficulties)
                sortByTick(d.solos);
        for (std::vector<uint32_t> *hits : {&chart.kicks, &chart.snares, &chart.crashes, &chart.drums, &chart.keys})
            std::sort(hits->begin(), hits->end());
        holdSyllables(chart);
    }
}
