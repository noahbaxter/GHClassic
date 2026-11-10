#pragma once

// What stage.cpp, band.cpp and venue.cpp share, for them only.

#include "custom/gh2_chart.h"
#include "custom/stage/stage.h"

#include <map>

namespace gh2::custom
{
    using TickMap = std::function<uint32_t(uint32_t)>;

    constexpr uint32_t kBeat = kGh2Resolution;

    // A gap of 8+ beats between notes ends a playing run.
    constexpr uint32_t kRestEndsRun = 8u * kBeat;

    // A sixteenth note.
    inline void addShortNote(midi::Track &track, uint32_t tick, uint8_t pitch) { addNote(track, tick, tick + kBeat / 4u, pitch); }

    // `ticks` as playing runs, each to its last tick.
    inline std::vector<Span> playingRuns(const std::vector<uint32_t> &ticks) { return runs(ticks, kRestEndsRun, 0u); }

    // Each change of the band's beat (its length in GH2's ticks, kBeat
    // twice for half, half for double) by GH2 tick, the first at 0.
    using BandBeatLengths = std::vector<std::pair<uint32_t, uint32_t>>;

    inline uint32_t beatAt(const BandBeatLengths &beats, uint32_t tick)
    {
        auto it = std::upper_bound(beats.begin(), beats.end(), tick,
                                   [](uint32_t t, const std::pair<uint32_t, uint32_t> &b) { return t < b.first; });
        return std::prev(it)->second;
    }

    enum class Mood
    {
        Verse,
        Chorus,
        Solo,
    };

    std::vector<uint32_t> ticksOf(const std::vector<Gem> &gems);

    // `ticks` in GH2 ticks, sorted, each once.
    std::vector<uint32_t> toGh2Sorted(const std::vector<uint32_t> &ticks, const TickMap &toGh2);

    // The chart's track a part's own texts and hand are on.
    const char *trackOf(Part part);

    const Chart::StageTrack *stageTrack(const Chart &chart, const char *name);

    // PART BASS as GH2's bassist's strumming hand reads it (bass_strum):
    // the bass part's frets each difficulty, an open on green, and its
    // slap where the chart says.
    midi::Track bassHand(const Chart &chart, const TickMap &toGh2);

    // What addStage's steps share: the lead part's ticks and playing
    // runs, the band's beat and bars, and the tracks being made.
    struct BandTracks
    {
        const StageIn &in;
        midi::Track &guitar, &events;
        std::vector<uint32_t> lead;
        std::vector<Span> playing;
        BandBeatLengths beats;
        std::vector<uint32_t> bars;
        std::vector<Chart::Cue> guitaristCues;
        midi::Track drums, bass, keys, singer, triggers;
    };

    // PART GUITAR's play state, then its fret hand: the chart's places
    // and maps, else made.
    void addGuitarist(BandTracks &band);

    // BAND DRUMS, playing while the drums part does, else with the lead:
    // its kicks and drum pattern. Returns the kicks.
    std::vector<uint32_t> addDrummer(BandTracks &band);

    // BAND BASS: a speaker pulse on each of the bass part's expert notes,
    // else on the kicks.
    void addBassist(BandTracks &band, const std::vector<uint32_t> &kicks);

    // BAND KEYS, playing where the chart's keys part does
    // (custom/song_files.cpp).
    void addKeys(BandTracks &band);

    // BAND SINGER: the chart's own play state, else each vocal phrase.
    void addSinger(BandTracks &band);

    struct Section
    {
        uint32_t tick;
        std::string name;
        double busy = 0.0;
        Mood mood = Mood::Verse;
    };

    // The chart's sections, else one every eight bars, each with how busy
    // the lead is in it and its mood: its name's, else a chorus where its
    // name's sections are on average kBusier than the median section.
    std::vector<Section> sectionsOf(const BandTracks &band);

    // EVENTS: each section's and solo's mood and lighting.
    void addMoods(BandTracks &band, const std::vector<Section> &sections);

    // A crash at each section start: every section where the drums are
    // charted, else all but verses.
    void addSectionCrashes(BandTracks &band, const std::vector<Section> &sections);

    // Where the chart has no wails, the guitarist wails through each
    // chorus's first four bars it plays in, as GH2's own charts mostly do.
    void addWails(BandTracks &band, const std::vector<Section> &sections);

    // TRIGGERS: the lighting's next keyframe each bar from a bar before
    // the lead.
    void addKeyframes(BandTracks &band);
}
