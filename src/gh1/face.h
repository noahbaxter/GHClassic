#pragma once

#include "addresses.h"
#include "formats/dtb.h"

#include <string>
#include <utility>
#include <vector>

class PS2Runtime;

namespace gh2
{
    namespace gh1
    {
        // An outfit's face_data from its GH1 archetype, for the morphs named
        // <outfit>_face.mrf and <outfit>_lashes.mrf. False if it lacks what
        // CharFace needs.
        bool addFace(const std::string &outfit, const dtb::Node &faceData);

        // When that song's singer has its mouth open, in seconds from
        // (start, end) in order: a face with an event_list shows them.
        void addSinging(const std::string &song, std::vector<std::pair<float, float>> open);
    }

    // Poses GH1 guitarists' faces as GH1's CharFace did, on GH2's RndMorph.
    void installGh1Face(PS2Runtime &runtime, const Addresses &addresses);
}
