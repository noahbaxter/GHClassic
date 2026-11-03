#pragma once

// How far the scan of the song folders has got, for the list to say so
// while it runs ({custom scan}, custom/command.h). The scan thread writes
// it, the UI reads it.

#include <atomic>
#include <cstddef>
#include <string>

namespace gh2::custom
{
    struct ScanStatus
    {
        std::atomic<size_t> seen{0u};     // songs this scan has found so far
        std::atomic<size_t> expected{0u}; // the last scan's, 0 on a first run
        std::atomic<bool> running{false};
    };

    inline ScanStatus &scanStatus()
    {
        static ScanStatus s_status;
        return s_status;
    }

    // What the list says while the scan runs, "" once it is done.
    inline std::string scanText()
    {
        const ScanStatus &s = scanStatus();
        if (!s.running)
            return {};
        const size_t seen = s.seen, expected = s.expected;
        return "SCANNING " + std::to_string(seen) + (expected > seen ? " / " + std::to_string(expected) : std::string());
    }
}
