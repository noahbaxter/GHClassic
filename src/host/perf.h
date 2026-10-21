#pragma once

// The frame times a run keeps: for the perf_overlay setting's panel in the
// picture's top left corner, and for --profiler's summary as the game
// closes.

#include "host/scene_renderer.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace gh2::perf
{
    // The perf_overlay setting's words.
    enum Level
    {
        kOff,
        kFps,   // frames a second and the frame's milliseconds
        kGraph, // and each of the last frames' as a bar, with the worst of them
        kFull,  // and where the frame went: the host's waits and the GPU's passes
    };

    // --profiler: this run writes frametimes.csv as [dev] frame_log
    // does, and prints summary() as it closes.
    void logThisRun();
    bool logging();

    class Times
    {
    public:
        // A frame presented, whether it shows a new game frame, by what it
        // waited for the game's frame, what recording its draws took, and the
        // GPU's passes.
        void add(bool isNew, float waitMs, float recordMs, const SceneStats &scene);

        // The panel for `level`, each of its pixels `scale` of the picture's
        // across: RGBA8, `width` by `height`. Empty for kOff.
        const std::vector<uint8_t> &draw(int level, uint32_t scale, uint32_t &width, uint32_t &height);

        // Every frame since the first, for the log.
        std::string summary() const;

    private:
        static constexpr size_t kRecent = 156; // the panel's width inside its edges

        std::chrono::steady_clock::time_point m_last{};
        float m_recent[kRecent]{}; // frame milliseconds, a ring
        size_t m_at = 0;
        uint64_t m_frames = 0;
        // What the panel reads, taken a few times a second so it can be read.
        float m_shownMs = 0.0f, m_shownWorst = 0.0f, m_shownWait = 0.0f, m_shownRecord = 0.0f;
        float m_shownGpu[std::size(kGpuSpans)]{};
        float m_wait = 0.0f, m_record = 0.0f;
        float m_gpu[std::size(kGpuSpans)]{};

        std::vector<float> m_all, m_allWait, m_allRecord, m_allGpu[std::size(kGpuSpans)];
        // How long each game frame stayed on screen: a stalled game thread
        // repeats the picture at full rate, which the frame times above
        // cannot see.
        std::chrono::steady_clock::time_point m_lastNew{};
        std::vector<float> m_allHeld;
        std::vector<uint8_t> m_small, m_pixels;
    };
}
