#include "dev/timing.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

namespace gh2::timing
{
    namespace
    {
        constexpr const char *kNames[kStatCount] = {
            "host_loop",        "wait_frame",        "wait_fence",      "wait_acquire",    "record",
            "submit",           "present_call",      "present_to_screen", "publish_to_screen", "game_frame",
            "frame_age",        "queue_behind_n",    "shown_again_n",   "pad_reads_n",     "event_to_poll",
            "poll_to_read",     "read_to_clock",     "clock_to_publish", "read_to_publish", "read_to_present",
            "event_to_present", "pull_interval",
            "pull_frames_n",    "pull_queued",       "clock_error",     "clock_age",       "clock_pull",
        };
        constexpr uint64_t kEveryNs = 2000000000ull;

        std::atomic<bool> s_on{false};
        std::mutex s_mutex;
        std::vector<double> s_samples[kStatCount];
        uint64_t s_reportedAt = 0;
        // The game thread's own.
        uint64_t s_pressEventNs = 0, s_pressReadNs = 0, s_lastFrameNs = 0;
        int s_padReads = 0;
        uint64_t s_readNs = 0, s_clockNs = 0; // the last pad read, and the first song clock read after it
    }

    void pressRead(uint64_t eventNs, uint64_t readNs)
    {
        s_pressEventNs = eventNs;
        s_pressReadNs = readNs;
    }

    void takePress(uint64_t &eventNs, uint64_t &readNs)
    {
        eventNs = s_pressEventNs;
        readNs = s_pressReadNs;
        if (readNs != 0u)
            span(kReadToPublish, readNs, now());
        s_pressEventNs = s_pressReadNs = 0;
    }

    void padRead()
    {
        ++s_padReads;
        s_readNs = now();
    }

    void clockRead()
    {
        if (s_readNs == 0u || s_clockNs != 0u)
            return;
        s_clockNs = now();
        span(kReadToClock, s_readNs, s_clockNs);
    }

    void gameFrame(uint64_t ns)
    {
        if (s_clockNs != 0u)
            span(kClockToPublish, s_clockNs, ns);
        s_clockNs = 0;
        if (s_lastFrameNs != 0)
            span(kGameFrame, s_lastFrameNs, ns);
        s_lastFrameNs = ns;
        sample(kPadReads, s_padReads);
        s_padReads = 0;
    }

    void enable()
    {
        s_on = true;
        s_reportedAt = now();
    }

    bool on() { return s_on.load(std::memory_order_relaxed); }

    uint64_t now() { return SDL_GetTicksNS(); }

    void sample(Stat stat, double value)
    {
        if (!on())
            return;
        std::lock_guard lock(s_mutex);
        s_samples[stat].push_back(value);
    }

    void note(const std::string &line)
    {
        if (on())
            std::cerr << "[timing] " << line << std::endl;
    }

    void report()
    {
        if (!on() || now() - s_reportedAt < kEveryNs)
            return;
        s_reportedAt = now();
        std::vector<double> taken[kStatCount];
        {
            std::lock_guard lock(s_mutex);
            for (int i = 0; i < kStatCount; ++i)
                taken[i].swap(s_samples[i]);
        }
        for (int i = 0; i < kStatCount; ++i)
        {
            std::vector<double> &v = taken[i];
            if (v.empty())
                continue;
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (const double x : v)
                sum += x;
            char line[160];
            std::snprintf(line, sizeof line, "%-16s n=%-5zu mean=%8.3f p50=%8.3f p95=%8.3f min=%8.3f max=%8.3f", kNames[i],
                          v.size(), sum / static_cast<double>(v.size()), v[v.size() / 2u], v[v.size() * 95u / 100u],
                          v.front(), v.back());
            std::cerr << "[timing] " << line << std::endl;
        }
    }
}
