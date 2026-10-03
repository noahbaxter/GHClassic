#pragma once

// Text beyond the locale's own, for tokens the host adds.

#include "addresses.h"

#include <string>

class PS2Runtime;

namespace gh2::locale
{
    // `token` localizes to `text`.
    void add(const std::string &token, const std::string &text);

    // After every add.
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
