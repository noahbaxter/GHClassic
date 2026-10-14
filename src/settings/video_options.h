#pragma once

#include "addresses.h"

class PS2Runtime;

namespace gh2
{
    void installVideoOptions(PS2Runtime &runtime, const Addresses &addresses);

    // The most samples a pixel the GPU draws colour and depth at, from the
    // host once it knows.
    void setMostMsaa(int most);
    // The samples a pixel the msaa setting comes to: the power of two at or
    // under it, and at or under that most.
    int msaaSamples();
}
