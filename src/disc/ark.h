#pragma once

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::ark
{
    // A content disc whose archive is searched after the game disc's, in the
    // order added. False, with the reason printed, if it cannot be read.
    bool addDisc(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
