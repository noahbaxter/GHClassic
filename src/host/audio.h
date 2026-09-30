#pragma once

#include <cstddef>
#include <cstdint>

namespace gh2
{
    // The rate the SPU2 mixes at, and so the rate a source renders at.
    constexpr uint32_t kAudioRate = 48000u;

    // Adds `frames` frames of interleaved stereo float into `out`. Called on
    // the audio thread. The source's clock is the device's: the synth ticks
    // as it is pulled.
    using AudioSource = void (*)(float *out, size_t frames);

    // The frontend's side: the default output device, or muted, a thread
    // pulling at the same rate and discarding it, so the synth still runs.
    void openAudio(bool mute);
    void closeAudio();

    // What is pulled. Silence until one is set; settable any time.
    void setAudioSource(AudioSource source);
}
