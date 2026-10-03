#pragma once

#include "addresses.h"

#include <cstddef>
#include <optional>
#include <string>

class PS2Runtime;

namespace gh2::ark
{
    // A content disc whose archive is searched after the game disc's, in the
    // order added. False, with the reason printed, if it cannot be read.
    bool addDisc(const std::string &path);

    // Which indexed disc boots that executable ("SLUS_215.86"), once
    // install has run.
    std::optional<size_t> discWithSerial(const std::string &serial);

    // Paths starting `as` are found as `source` plus the rest, on that disc
    // alone: "char/punk3/anims/" -> 80s "char/punk1/anims/".
    void rename(const std::string &as, size_t disc, const std::string &source);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
