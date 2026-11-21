#pragma once

// GHClassic.log, a player's run as they send it in: each line stamped with
// the time since the start, whole lines only whichever thread wrote them,
// and a line for a crash. Each run is appended under its [system] line, the
// oldest dropped whole to keep the file small enough to post on Discord,
// since the run a player reports is often one before the last. Player logs
// are posted publicly: the home folder in any line is written as ~.

#include <cstdint>
#include <string>

namespace gh2::log
{
    // stderr and stdout to `path`, stamped. Once, before anything else is
    // written.
    void open(const std::string &path);
    // Writing to GHClassic.log, rather than a terminal or a tool's pipe.
    bool toFile();
    // The log's first line: this build, the OS and its language, CPU and
    // memory, and when.
    void system();
    // The memory this process holds now, resident. Any thread.
    uint64_t memoryBytes();

    // What only a developer reads: every font baked, every menu loop. Kept
    // out of GHClassic.log unless the run has --verbose; everywhere else on.
    void setVerbose();
    bool verbose();

    // UI goto_screen went to it: a line, where every line after it
    // happened, and the screen a crash line names.
    void screen(const char *name);

    // The run ended as it should: its last line, which tells a crash or a
    // kill from a close.
    void closed();
}
