#pragma once

// The pool of light under the guitarist in one of GH1's venues.
//
// GH1 has a VenueSpotLight for the first guitarist (Arena::SetupSpotlights,
// GH1 0x168808): the room's spotlight01.lit, which its script moves from one
// walk spot to the next, and floorspot_char.mesh, a square of the room's
// floorspot_glow.mat. Each frame it turns the Light, and the beam that hangs
// under it, to where the guitarist stands and lays the mesh on the floor
// there, drawn out along the way the light falls (VenueSpotLight::Poll, GH1
// 0x179420). GH2's world does none of it, so a GH1 venue's script
// (gh1/scripts.cpp) names the three and they are moved here as the world is
// drawn.
//
//   {floor_spot <mesh> <light> <character>}  that mesh is the pool that
//                                            light throws under that character

#include "addresses.h"

class PS2Runtime;

namespace gh2::floor_spot
{
    void install(PS2Runtime &runtime, const Addresses &addresses);
}
