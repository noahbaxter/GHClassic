#pragma once

// --record: the game as a video, its picture piped to ffmpeg as it is drawn
// and its mixed sound beside it, both on the game's clock, so a run at any
// --speed comes out in step and at the game's own pace.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gh2::record
{
    // Before the frontend starts: the .mp4 to write.
    void setPath(const std::string &path);
    bool on();

    // Held, nothing is written: a scenario's (record off), and (record on)
    // carries on where it left off.
    void hold(bool held);

    // A drawn frame, RGBA, at that time on the game's clock. The first starts
    // the recording; a frame late for its place in the video repeats the one
    // before, so the picture keeps time with the sound.
    void frame(const uint8_t *rgba, uint32_t width, uint32_t height, std::chrono::steady_clock::time_point at,
               std::chrono::nanoseconds period);

    // Interleaved stereo at kAudioRate, as mixed, from when the first frame
    // starts the recording.
    void audio(const float *stereo, size_t frames);

    // The two joined into the .mp4, once the game stops.
    void finish();
}
