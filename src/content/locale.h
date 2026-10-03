#pragma once

// Text beyond the locale's own, for tokens the host adds.

#include "addresses.h"

#include <string>

class PS2Runtime;
struct R5900Context;

namespace gh2::locale
{
    // `token` localizes to `text`.
    void add(const std::string &token, const std::string &text);

    // The locale's files read again as the archive now gives them
    // (Locale::Terminate and Locale::Init, as SetSystemLanguage does at
    // 0x2a88a0), with every added token back in.
    void reload(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // After every add.
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
