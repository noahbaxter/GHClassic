#pragma once

// A custom song's band, lights and camera, made from its chart, as a GH2
// chart carries them (config/midi_parsers.dta, world_objects_worldbase.dta).
// Each member's [play] and [idle] are its chart track's own where it has
// them (Rock Band's [mellow], [idle_realtime] and the like as those), else
// made, with its tempo, wail and face texts as written:
//
//   PART GUITAR  the guitarist, playing where the part does; [wail_on] and
//                [wail_off] over a chorus's first four bars; its fret hand's
//                place (40 to 59) and [map ...]s, the chart's, else one place
//                a part of the song (its sections, else the bars it
//                repeats), the same for each part alike, busier higher,
//                solos and taps highest, and a hand map a part by what it
//                plays: chords, runs and trills, solos, the rest
//   PART BASS    the bass part's frets for the bassist's strumming hand, and
//                its [map StrumMap_SlapBass]
//   BAND BASS    the bassist, and 36 a speaker pulse on each hit: the bass
//                part's, else the kicks, else the beat
//   BAND DRUMS   the drummer, 36 a kick (the drums' expert kicks, else beats
//                1 and 3 of each bar the band plays) and 37 a crash (the
//                chart's crashes, else at each section); its beat the chart's
//                own, else by where the snares fall each bar: [play] (the
//                snare on 2 and 4), [allbeat], [double_time], [half_time],
//                [nobeat]; with no drums charted, [play]'s alone
//   BAND SINGER  the singer, by when there is singing; with none in the
//                chart, the band's playing but its solos
//   BAND KEYS    the keyboard player, by the keys part, where it has one
//   EVENTS       each section's mood: [verse], [chorus] or [solo] for the
//                camera and lighting, and a [lighting (...)]. A section's
//                mood is by its name less its number ("Chorus 2" as
//                "Chorus 1"): chorus, solo or verse where the name says,
//                else by how busy the part is against the rest of the song.
//                With no sections, one every eight bars. A chart's own
//                [lighting (...)] and moods each in place of those made, and
//                its crowd's tempo and lighters and its band's [band_jump],
//                [sync_head_bang] and [sync_wag] as written.
//   TRIGGERS     48 each bar, the lighting's next keyframe, for the lighting
//                made
//   GH1 UPCOMING GH1's venues' drum cue, a beat ahead of BAND DRUMS' [play]
//                and [idle]
//
// A chart made for GH2 keeps what its own BAND tracks and TRIGGERS write,
// lane by lane: when a member plays, its tempo, the drummer's beat, each
// note's pitch (the keyframes as one). What a lane the chart writes leaves
// out stays out; the lanes it doesn't write are made, around its own (the
// drummer's kicks where its BAND DRUMS has it play, the bass's hits on its
// kicks). Where any of its tracks sets a tempo, none is made.
//
// The band and crowd play to the chart's beat, so a chart past 210 BPM (or
// charted at twice its song's tempo) plays them at [half_tempo], one under
// 80 at [double_tempo], each member's and EVENTS' [crowd_..._tempo], and a
// bar above is the band's: two of the chart's at half, half of one at
// double.
//
// Every tick is GH2's (480 a beat).

#include "custom/chart.h"
#include "formats/midi.h"

#include <cstdint>
#include <functional>
#include <string>

namespace gh2::custom
{
    struct StageIn
    {
        const Chart &chart;
        const Difficulty &lead; // PART GUITAR's expert
        std::function<uint32_t(uint32_t)> toGh2; // chart tick to GH2 tick
        uint32_t end = 0u; // the song's [end]
        Part part = Part::Guitar; // the part PART GUITAR is, its texts and hand the chart's
        // Where given, when the singer sings, by GH2's ticks: the chart's
        // phrases, else the band's playing but its solos.
        std::vector<Span> *singing = nullptr;
    };

    // `guitar` and `events` (PART GUITAR, EVENTS) with the band's texts
    // added; the band's own tracks, a chart made for GH2's where it has
    // them, and GH1's venues' drum cue.
    std::vector<midi::Track> addStage(const StageIn &in, midi::Track &guitar, midi::Track &events);

    // GH1's venues' drum cue, which GH1 sent its venue a beat ahead of its
    // drum_on and drum_off (gh1/songs.cpp, gh1/scripts.cpp): 60 a beat
    // before each of BAND DRUMS' [play], 61 before each [idle].
    inline constexpr const char *kUpcomingTrack = "GH1 UPCOMING";
    midi::Track upcomingDrums(const midi::Track &bandDrums, uint32_t ticksPerBeat);

    // GH2 plays its band's clips and crowd to the chart's beat. Its own songs
    // play at their own tempo from 88 to 201 BPM, Tattooed Love Boys (242)
    // at half, Red Lottery (61) at double; a chart's beat past these is
    // played at half or double.
    inline constexpr double kBandFastest = 210.0, kBandSlowest = 80.0;
    // A tempo as the band plays it.
    inline double bandBpm(double bpm) { return bpm > kBandFastest ? bpm / 2.0 : bpm < kBandSlowest ? bpm * 2.0 : bpm; }

    // The made hand maps are GH2's (config/midi_parsers.dta) with this after
    // their name: each the same but an open played as finger_open rather
    // than a power chord (custom/install.cpp).
    inline constexpr const char *kOpenHandMap = "_Open";

    // A section's name as its mood is picked by: lowercase, '_' a space, its
    // number and a trailing letter or two ("Verse 1A", "chorus_2b") left off.
    std::string sectionBaseName(const std::string &section);
}
