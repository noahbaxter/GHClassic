#pragma once

// The player's GH2 disc image, and the boot executable on it, which must be
// the one this build was recompiled from (GHC_RETAIL_SHA1).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gh2::disc
{
    constexpr const char *kFolder = "PUT_DISC_HERE";

    // A PUT_DISC_HERE folder beside the executable: a portable install, which
    // keeps its settings and saves there too.
    bool portable();

    // Where the player puts their disc image: beside the executable when
    // portable, else in the user data directory. Made if missing.
    std::filesystem::path folder();

    // The image's boot executable if it is this build's, else empty and why.
    std::vector<uint8_t> bootElf(const std::filesystem::path &image, std::string &why);

    // The first image in folder() with this build's executable, else empty
    // and why each image there was turned down.
    std::filesystem::path find(std::string &why);
}
