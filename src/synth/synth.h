#pragma once

#include "addresses.h"

#include <cstddef>

class PS2Runtime;

namespace gh2
{
    void installSynth(PS2Runtime &runtime, const Addresses &addresses);

    namespace synth
    {
        class Timeline;

        // The mix, as the audio device pulls it: see host/audio.h.
        void render(float *out, size_t frames);
        // Sounds played at the song's time (synth/timeline.h).
        Timeline &timeline();
        // The song's audio is made at `percent` speed with its pitch kept,
        // and keeps it at any speed it plays at; 0: it plays as it is.
        void keepPitch(int percent);
    }
}
