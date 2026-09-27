#pragma once

#include <cstdint>

namespace gh2
{
    // Where each hooked function lives in one executable. Every hook takes
    // its address from here, so a second executable is a second table.
    struct Addresses
    {
        uint32_t entry;
        uint32_t psRndFlushPacket;
        uint32_t psRndBeginDrawing;
        uint32_t psRndEndDrawing;
        uint32_t psMeshSync;
        uint32_t psMeshCopy;
        uint32_t psMeshDestroy;
        uint32_t psMeshDrawShowing;
        uint32_t psMatSelect;
        uint32_t worldXfm;       // RndTransformable::WorldXfm
        uint32_t playMovie;
        // Data
        uint32_t rndCamCurrent;  // RndCam::sCurrent
        uint32_t defaultMat;     // the RndMat a mesh without one draws with
    };

    // Guitar Hero II (USA), SLUS-21447.
    inline constexpr Addresses kSlus21447{
        .entry = 0x100bf0u,
        .psRndFlushPacket = 0x3d7d58u,
        .psRndBeginDrawing = 0x19af38u,
        .psRndEndDrawing = 0x19b018u,
        .psMeshSync = 0x3d4f08u,
        .psMeshCopy = 0x19dc38u,
        .psMeshDestroy = 0x19dd88u,
        .psMeshDrawShowing = 0x3d88d8u,
        .psMatSelect = 0x3d8348u,
        .worldXfm = 0x3d8ea0u,
        .playMovie = 0x21bb60u,
        .rndCamCurrent = 0x3de348u,
        .defaultMat = 0x3da4f0u,
    };
}
