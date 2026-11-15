#pragma once

// Sounds played at a song's time, to the sample: a click on a note, a
// metronome's beat. The song's time is its stream's own, where its voice is
// in its data as each frame is mixed (Module::render), so a sound lands on
// the music whatever the frame rate, a hitch, or the audio's latency (both
// reach the player through the same mix).
//
// The game's thread adds sounds and schedules them; the audio thread mixes.

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace gh2::synth
{
    class Timeline
    {
    public:
        // A mono sound at `rate`; its id for schedule.
        int addSound(const std::vector<int16_t> &pcm, uint32_t rate);

        // The stream whose time is the song's: its id, its sample rate and
        // the song's time at its first sample (StreamEE's own, synth.cpp).
        // None: nothing plays.
        struct Clock
        {
            uint32_t stream = 0u;
            uint32_t rate = 0u;
            float startMs = 0.0f;
        };
        void setClock(std::optional<Clock> clock);
        std::optional<Clock> clock() const;

        // `sound` at the song's `ms`, at `gain` and `pan` (-1 left to 1
        // right, as an SPU voice's: the far side down, the near one kept).
        // One the music has passed by more than kLateMs when it is first
        // mixed is dropped.
        void schedule(double ms, int sound, float gain, float pan = 0.0f);
        // Every one not yet started (the song jumped).
        void clear();
        // How many have started since the game began.
        uint64_t started() const;

        // Audio thread: `frames` of stereo into `out`, the song at `ms` at
        // its first frame and moving `msPerFrame` a frame (0: standing).
        void mix(float *out, size_t frames, double ms, double msPerFrame);

    private:
        static constexpr double kLateMs = 30.0;

        struct Event
        {
            double ms;
            int sound;
            float left, right; // gains
        };
        struct Playing
        {
            int sound;
            size_t at; // the next sample
            float left, right;
        };

        mutable std::mutex m_mutex;
        std::vector<std::vector<float>> m_sounds; // each at the mix's 48 kHz
        std::optional<Clock> m_clock;
        std::vector<Event> m_events; // by ms
        std::vector<Playing> m_playing;
        uint64_t m_started = 0u;
    };
}
