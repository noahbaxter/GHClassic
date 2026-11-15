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
    }
}
