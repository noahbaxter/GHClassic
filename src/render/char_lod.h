#pragma once

// The high_detail setting: every character drawn as its nearest model,
// however small it is in the picture.

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installCharLod(PS2Runtime &runtime, const Addresses &addresses);
}
