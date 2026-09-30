#pragma once

#include "addresses.h"

#include <cstddef>

class PS2Runtime;

namespace gh2
{
    void installSynth(PS2Runtime &runtime, const Addresses &addresses);

    namespace synth
    {
        // The mix, as the audio device pulls it: see host/audio.h.
        void render(float *out, size_t frames);
    }
}
