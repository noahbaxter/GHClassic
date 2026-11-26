#pragma once

// The player's GH2 disc image, and the boot executable on it, which must be
// the one this build was recompiled from (GHC_RETAIL_SHA1), or Europe's,
// which the build's patch turns into it.

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

    // The image's boot executable if it is this build's (Europe's patched
    // into it), else empty and why.
    std::vector<uint8_t> bootElf(const std::filesystem::path &image, std::string &why);

    // The images in folder(): settings.ini's [disc] prefer first, then by
    // name.
    std::vector<std::filesystem::path> images();

    // The first image in folder() with this build's executable, else empty.
    // Why each image there was turned down goes to the log.
    std::filesystem::path find();

    // What the player is told with no game disc: where it goes, then the
    // other games' discs found there.
    std::string missing();
}
