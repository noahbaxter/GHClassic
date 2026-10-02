#pragma once

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::save
{
    // Read and write `path` in place of the user data directory's save.bin.
    void usePath(const std::string &path);
    // Load GH2's save from this PCSX2 card (.ps2) instead, making it save.bin.
    void importCard(const std::string &path);
    // Write GH2's save to this PCSX2 card after the load and each save,
    // formatting a new card if there is none. settings.ini's export_card does
    // the same to GHClassic.ps2 beside save.bin.
    void exportCard(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
