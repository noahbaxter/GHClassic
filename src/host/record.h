#pragma once

// --record: the game as video clips, each frame piped to ffmpeg as it is
// drawn and the mixed sound written beside it, both on the game's clock, so
// a run at any --speed comes out in step and at the game's own pace. Each
// clip is <name>.ts and <name>.f32 in the folder given, which tools/ghc.py
// record joins once the game is gone. A transport stream plays cut short,
// so a run killed mid-clip keeps what it drew.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gh2::record
{
    // Before the frontend starts: the folder the clips go in.
    void setPath(const std::string &dir);

    // A clip from the next frame, under that name or else its number, ending
    // any clip before it. A run records one clip from its first frame unless
    // stopped before it.
    void start(const std::string &name = {});
    // The clip so far, ended, and none until the next start.
    void stop();

    // A clip is on, so frames are wanted.
    bool wanted();

    // A drawn frame, RGBA and shown at that aspect, at that time on the
    // game's clock. One not the clip's size (a movie's, at its own) is fitted
    // into it. A frame late for its place in the clip repeats the one before,
    // so the picture keeps time with the sound.
    void frame(const uint8_t *rgba, uint32_t width, uint32_t height, float aspect,
               std::chrono::steady_clock::time_point at, std::chrono::nanoseconds period);

    // Interleaved stereo at kAudioRate, as mixed, into the clip on at its
    // place on the game's clock.
    void audio(const float *stereo, size_t frames);

    // The last clip ended and every frame written, once the game stops.
    void finish();
}
