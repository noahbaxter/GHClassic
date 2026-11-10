#pragma once

// A custom song's band, lights and camera, made from its chart, as a GH2
// chart carries them (config/midi_parsers.dta, world_objects_worldbase.dta):
//
//   PART GUITAR  [play] and [idle], the guitarist playing where the part does
//   BAND BASS    the same for the bassist, and 36 a speaker pulse on each hit:
//                the bass part's, else the kicks, else the beat
//   BAND DRUMS   the same for the drummer, 36 a kick (the drums' expert kicks,
//                else every beat the band plays) and 37 a crash at each section
//   BAND SINGER  the same for the singer, by when there is singing
//   BAND KEYS    the same for the keyboard player, by the keys part, where
//                it has one
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
    };

    // `guitar` and `events` (PART GUITAR, EVENTS) with the band's texts
    // added; the band's own tracks.
    std::vector<midi::Track> addStage(const StageIn &in, midi::Track &guitar, midi::Track &events);

    // A section's name as its look's: lowercase, '_' a space, its number and
    // a trailing letter or two ("Verse 1A", "chorus_2b") left off.
    std::string sectionBaseName(const std::string &section);
}
