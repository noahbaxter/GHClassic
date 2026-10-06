#pragma once

// Another run's memory, drawn here: the retail game's RAM as PCSX2 held it
// (tools/pcsx2.py, the scenario step (ram name)) replaces this game's, so the
// next frames draw exactly the state retail was in, characters, crowd and
// all. Both run the same executable, so everything is at the same addresses.
//
// Only drawing survives it. The threads, the sound and the files behind the
// memory are still this run's, so from then on the main loop's polls
// (SystemPoll, the synth's, UIManager::Poll) do nothing and each pass draws
// the same frame again. Meshes and textures, which the host holds by the
// address they synced at, are read again as they are drawn: a mesh out of
// its packet (dev/mesh_packet.h), a texture out of its bitmap
// (render/texture_capture.h).

#include "addresses.h"

#include <cstdint>
#include <string>

class PS2Runtime;

namespace gh2::transplant
{
    // False, with nothing changed, if the file is not a RAM image.
    bool load(uint8_t *rdram, const std::string &path);
    // Whether the memory is another run's.
    bool active();

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
