#pragma once

// A custom song's chart, from notes.mid or notes.chart, with everything a
// five-fret chart can say: open notes, a sustain per lane (so one held
// through others, or chords whose lanes end apart), forced strums and
// HOPOs, taps, star power and solos. GH2's parser takes only some of it;
// the rest reaches the engine beside the MIDI (custom/gh2_chart.h).

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::custom
{
    constexpr int kOpen = 5; // the lane past orange

    enum class Kind : uint8_t
    {
        Strum,
        Hopo,
        Tap,
    };

    // Every lane struck at one tick.
    struct Gem
    {
        uint32_t tick = 0u;
        uint8_t lanes = 0u; // bit n lane n, green 0 to orange 4, open 5
        std::array<uint32_t, 6> length{}; // each lane's note length, 0 none
        // GH2's natural kind (a single note on no fret of the gem before,
        // under the song's threshold), unless the chart forces or flips it.
        Kind kind = Kind::Strum;
        bool forced = false; // its kind is the chart's: a forced HOPO or strum, or a tap
        bool flip = false;   // the running engine's natural kind flipped (.chart's 5)
        bool inLanePhrase = false, lanePhraseStart = false; // in a trill or tremolo phrase, and its first note

        bool chord() const { return (lanes & (lanes - 1u)) != 0u; }
        // Its lanes don't all end together.
        bool split() const;
        uint32_t longest() const;
    };

    struct Span
    {
        uint32_t tick = 0u, length = 0u;
    };

    // `ticks` (any order) as runs, each a span from its first to past its
    // last by `tail`, a new one where two are more than `gap` apart.
    std::vector<Span> runs(std::vector<uint32_t> ticks, uint32_t gap, uint32_t tail);

    struct Difficulty
    {
        std::vector<Gem> gems; // by tick
        std::vector<Span> starPower, solos;
        std::array<std::vector<Span>, 2> faceOff; // each player's phrases
        std::vector<Span> bigNotes, rockEndings;  // GH2's note 110, Rock Band's big rock endings
        // Tremolo and trill phrases as marked (126 and 127, .chart's S 65
        // and S 66), and each phrase's bands, one per fret: its first note
        // there to its last.
        std::vector<Span> tremolos, trills;
        struct Band
        {
            uint32_t tick = 0u, length = 0u;
            int lane = 0;
        };
        std::vector<Band> bands; // by tick
    };

    enum class Part : uint8_t
    {
        Guitar,
        Coop,   // PART GUITAR COOP, .chart's DoubleGuitar
        Rhythm,
        Bass,
    };
    inline constexpr const char *kPartNames[] = {"guitar", "coop", "rhythm", "bass"};
    // Whether `parts`, bit n Part n, has that part.
    inline bool hasPart(uint8_t parts, Part part) { return parts & (1u << int(part)); }

    struct Tempo
    {
        uint32_t tick = 0u, usPerBeat = 500000u;
    };

    struct Chart
    {
        uint32_t resolution = 192u; // ticks a beat
        std::vector<Tempo> tempos;  // by tick, one at 0 at least
        std::vector<std::pair<uint32_t, std::array<uint8_t, 2>>> signatures; // numerator, denominator's log2
        std::vector<std::pair<uint32_t, std::string>> sections; // practice names, "Verse 1"
        std::map<Part, std::array<Difficulty, 4>> parts; // easy first
        // PART DRUMS' or [ExpertDrums]' expert: its kicks, and every hit
        // with them, each by tick.
        std::vector<uint32_t> kicks, drums;
        // When someone sings: PART VOCALS' phrases (105, 106), else runs
        // of its notes or lyrics; .chart's [Events] phrase_start and
        // phrase_end, else runs of its lyrics.
        std::vector<Span> vocals;
        // .chart's [Song] Offset, in seconds: the audio's lead.
        double offset = 0.0;
        // The song's own HOPO threshold at 480 a beat, as GH2's songs.dta
        // (hopo_threshold N); none for the engine's own.
        std::optional<uint32_t> hopoThreshold;

        double seconds(uint32_t tick) const;
    };

    // GH2's own natural HOPO threshold, at 480 a beat (beatmatcher.dta).
    constexpr uint32_t kGh2HopoThreshold = 170u;

    // song.ini's say in how a chart reads. A sustain's cutoff is the
    // running engine's, whatever the song says (sustain_cutoff_threshold).
    struct Options
    {
        // The song's natural HOPO threshold in its chart's ticks; half a
        // beat with eighthnote_hopo. Rips stamp GH2's 170 on every song,
        // GH1's too, so that is none: the engine's own.
        std::optional<uint32_t> hopoFrequency;
        bool eighthNoteHopo = false;
        // Star power on note 103 rather than 116 (multiplier_note).
        bool starPowerOn103 = false;
    };

    std::optional<Chart> parseMid(const std::vector<uint8_t> &bytes, const Options &options, std::string &why);
    std::optional<Chart> parseChart(const std::string &text, const Options &options, std::string &why);
    // A song's chart file by its name: a .mid as parseMid reads it, else as
    // parseChart.
    std::optional<Chart> parseSongChart(const std::string &name, const std::string &bytes, const Options &options,
                                        std::string &why);
}
