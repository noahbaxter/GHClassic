#pragma once

// How a venue's flat crowd is drawn.
//
// A WorldCrowd draws each flat member on a card under one material its
// constructor makes (0x268fe0, kept at +0xb4): solid, cut where the texel's
// alpha is 0. GH1 blends its cards by that alpha (FormFlatCrowd, GH1
// 0x170160), which leaves a member's edge soft where a card is near the
// camera. A crowd in one of GH1's venues is given GH1's blend.

#include "addresses.h"

class PS2Runtime;

namespace gh2::crowd_cards
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
