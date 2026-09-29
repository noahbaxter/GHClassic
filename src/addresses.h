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
        uint32_t psMeshFixVerts;
        uint32_t psMeshDestroy;
        uint32_t psMeshDrawShowing;
        uint32_t psMultiMeshDrawShowing;
        uint32_t psMatSelect;
        uint32_t psEnvironSelect;
        uint32_t psTexSyncBitmap;
        uint32_t psTexDestroy;
        uint32_t worldXfm;       // RndTransformable::WorldXfm
        uint32_t playMovie;
        uint32_t optionsSyncVideo; // Options::SyncVideoOptions
        uint32_t ctlClientPoll;
        // Data
        uint32_t rndCamCurrent;  // RndCam::sCurrent
        uint32_t defaultMat;     // the RndMat a mesh without one draws with
        uint32_t rndEnvironCurrent; // RndEnviron::sCurrent
        uint32_t synthServerHandler; // the handler CtlServerInit stores
        uint32_t synthServerBuffer;  // its RPC server's receive buffer
        // RndBitmap::PixelOffset's swizzle tables, by (y / 4) & 1: 64 bytes
        // each for 8bpp, 128 for 4bpp.
        uint32_t swizzle8[2];
        uint32_t swizzle4[2];
    };

    // Guitar Hero II (USA), SLUS-21447.
    inline constexpr Addresses kSlus21447{
        .entry = 0x100bf0u,
        .psRndFlushPacket = 0x3d7d58u,
        .psRndBeginDrawing = 0x19af38u,
        .psRndEndDrawing = 0x19b018u,
        .psMeshSync = 0x3d4f08u,
        .psMeshFixVerts = 0x19dbb8u,
        .psMeshDestroy = 0x19dd88u,
        .psMeshDrawShowing = 0x3d88d8u,
        .psMultiMeshDrawShowing = 0x1a2f00u,
        .psMatSelect = 0x3d8348u,
        .psEnvironSelect = 0x1a2060u,
        .psTexSyncBitmap = 0x1a13a8u,
        .psTexDestroy = 0x1a0f18u,
        .worldXfm = 0x3d8ea0u,
        .playMovie = 0x21bb60u,
        .optionsSyncVideo = 0x10db80u,
        .ctlClientPoll = 0x22dc68u,
        .rndCamCurrent = 0x3de348u,
        .defaultMat = 0x3da4f0u,
        .rndEnvironCurrent = 0x3de358u,
        .synthServerHandler = 0x3de454u,
        .synthServerBuffer = 0x484340u,
        .swizzle8 = {0x3de1c8u, 0x3de208u},
        .swizzle4 = {0x3de248u, 0x3de2c8u},
    };
}
