#pragma once

// A custom song's band, lights and camera, made from its chart, as a GH2
// chart carries them (config/midi_parsers.dta, world_objects_worldbase.dta).
// Each member's [play] and [idle] are its chart track's own where it has
// them (Rock Band's [mellow], [idle_realtime] and the like as those), else
// made, with its tempo, wail and face texts as written:
//
//   PART GUITAR  the guitarist, playing where the part does; [wail_on] and
//                [wail_off] over a chorus's first four bars; its fret hand's
//                place (40 to 59) and [map ...]s, the chart's, else the hand
//                at the lowest fret played
//   PART BASS    the bass part's frets for the bassist's strumming hand, and
//                its [map StrumMap_SlapBass]
//   BAND BASS    the bassist, and 36 a speaker pulse on each hit: the bass
//                part's, else the kicks, else the beat
//   BAND DRUMS   the drummer, 36 a kick (the drums' expert kicks, else every
//                beat the band plays) and 37 a crash at each section; its
//                beat by where the snares fall each bar: [allbeat],
//                [double_time], [half_time], [nobeat]
//   BAND SINGER  the singer, by when there is singing
//   BAND KEYS    the keyboard player, by the keys part, where it has one
//   EVENTS       each section's look: [verse], [chorus] or [solo] for the
//                camera and lighting, and a [lighting (...)]. A section is
//                its look by its name less its number ("Chorus 2" as
//                "Chorus 1"): chorus, solo or verse where the name says,
//                else by how busy the part is against the rest of the song.
//                With no sections, one every eight bars.
//   TRIGGERS     48 each bar, the lighting's next keyframe
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
    };

    // `guitar` and `events` (PART GUITAR, EVENTS) with the band's texts
    // added; the band's own tracks.
    std::vector<midi::Track> addStage(const StageIn &in, midi::Track &guitar, midi::Track &events);

    // A section's name as its look's: lowercase, '_' a space, its number and
    // a trailing letter or two ("Verse 1A", "chorus_2b") left off.
    std::string sectionBaseName(const std::string &section);
}
