#pragma once

// --timing: where time goes between a press, the sound and the picture.
// Each stage's delay is sampled where it happens and printed every two
// seconds as count, mean, median, 95th percentile and most, in ms unless
// the name says otherwise.

#include <cstdint>
#include <string>

namespace gh2::timing
{
    enum Stat
    {
        kHostLoop,     // one host frame() to the next: how often input is polled
        kWaitFrame,    // waiting on the game for a frame
        kWaitFence,    // waiting on the GPU for the frame two back
        kWaitAcquire,  // waiting on the swapchain for an image
        kRecord,       // recording the frame's draws
        kSubmit,       // inside vkQueueSubmit
        kPresentCall,  // inside vkQueuePresentKHR
        kPresentToScreen, // the present call returning to the display showing it, as the driver reports
        kPublishToScreen, // a frame's publish to the display showing it
        kGameFrame,    // one game frame to the next
        kFrameAge,     // a frame's publish to its present call returning
        kQueueBehind,  // frames still queued behind the one taken (count)
        kShownAgain,   // host frames that showed the last game frame again (0 or 1)
        kPadReads,     // scePadRead calls a game frame (count)
        kEventToPoll,  // the OS's time for a press to the host poll that saw it
        kPollToRead,   // that poll to the game's pad read that saw it
        kReadToClock,    // any pad read to the game next reading the song clock, which judges it
        kClockToPublish, // that reading to its frame's publish
        kReadToPublish,  // that read to the publish of the frame drawn after it
        kReadToPresent,  // that read to the present call of the frame drawn after it
        kEventToPresent, // the OS's time for a press to that present call
        kPullInterval,   // one audio pull to the next
        kPullFrames,     // sample frames asked for in a pull (count)
        kPullQueued,     // audio already queued in SDL's stream at a pull
        kClockError,     // song clock less the audio's position, at each lock
        kClockAge,       // age of the position report the lock used
        kClockPull,      // how far the lock moved the clock
        kStatCount
    };

    void enable();
    bool on();
    // --present-mode and --swap-images, to measure with: a VkPresentModeKHR
    // in place of FIFO, and the swapchain's least images. -1 leaves each.
    inline int presentMode = -1, swapImages = -1;
    // --fullscreen: the window starts fullscreen.
    inline bool fullscreen = false;
    // Nanoseconds on SDL's clock, the one its events are stamped with.
    uint64_t now();
    void sample(Stat stat, double value);
    inline void span(Stat stat, uint64_t from, uint64_t to)
    {
        sample(stat, static_cast<double>(static_cast<int64_t>(to - from)) / 1.0e6);
    }
    // The game's pad read saw a press: when the OS said it happened, 0 if
    // it did not say, and when it was read. Game thread.
    void pressRead(uint64_t eventNs, uint64_t readNs);
    // The press read since the last frame, for the frame being published
    // to carry: zeros if none. Game thread.
    void takePress(uint64_t &eventNs, uint64_t &readNs);
    // A pad read, the song clock read (PlayerMatcher::GetSongMs), and a
    // game frame published at `ns`. Game thread.
    void padRead();
    void clockRead();
    void gameFrame(uint64_t ns);
    // One line, once.
    void note(const std::string &line);
    // Prints what was sampled, every two seconds. Host thread.
    void report();
}
