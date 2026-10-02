#pragma once

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::save
{
    // Read and write `path` in place of the user data directory's save.bin.
    void usePath(const std::string &path);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
