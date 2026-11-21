#pragma once

// A line in the log for each song played: what it was as it starts, and as
// it ends how it ran. Frame times, the game frames held up past a hitch,
// how far the song clock strayed from the sound, and the sound late. What
// a player calls stutter, lag or notes out of time, in numbers, with what
// the game thread waited on texture reads and the memory held at the end,
// to show what grows over a career. A song frozen 3 s gets its line while
// it is still frozen. Always on: fixed counters, no allocation per frame.

#include <cstdint>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace gh2::song_log
{
    // The script command that names the song as it starts.
    void install();
    // The screen GH2 went to: a song starts on its game screen and ends on
    // any but that and its pause menus. Game thread.
    void screen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &name);
    // GamePanel::SetGameOver: the song won or failed.
    void gameOver(bool won);

    // A frame presented, showing a new game frame or the last again: its
    // GPU passes (of a frame a few back), recording its draws, and its
    // waits for the game thread's frame and for the display. Host thread.
    void present(bool isNew, float gpuMs, float recordMs, float gameWaitMs, float displayWaitMs);
    // What reads the GPU memory the renderer holds, for each song's end
    // line; nullptr once it is gone. Host thread.
    void gpuMemory(uint64_t (*bytes)(void *), void *of);
    // The song clock locked to the sound: how far apart, and how far it was
    // pulled. Game thread.
    void clock(float errorMs, float pullMs);
    // The sound device asked for more, `gapMs` after it last did, its
    // buffer `periodMs`. Audio thread.
    void audioPull(float gapMs, float periodMs);
}
