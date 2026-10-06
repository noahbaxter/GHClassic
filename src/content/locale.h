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

    // The Locale's table (its first word, 0x51f1b8) becomes `game`'s: the
    // locale's files as the archive gives them then, read the first time
    // (Locale::Init, 0x2cb798) with every added token in, and kept outside
    // the game's heap (script::park). No kept table is let go, so text the
    // game still points at stays.
    void enter(const std::string &game, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // After every add.
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
