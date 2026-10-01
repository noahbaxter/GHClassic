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
        uint32_t psRndDrawRect;
        uint32_t psMeshSync;
        uint32_t psMeshFixVerts;
        uint32_t psMeshDestroy;
        uint32_t psMeshDrawShowing;
        uint32_t psMultiMeshDrawShowing;
        uint32_t psParticleSysDrawShowing;
        uint32_t updateRelativeXfm; // RndParticleSys::UpdateRelativeXfm
        uint32_t psEnvironSelect;
        uint32_t psTexSyncBitmap;
        uint32_t psTexDestroy;
        uint32_t rndTextDrawShowing;
        uint32_t worldXfm;       // RndTransformable::WorldXfm
        uint32_t setWorldXfm;    // RndTransformable::SetWorldXfm
        uint32_t playMovie;
        uint32_t optionsSyncVideo; // Options::SyncVideoOptions
        uint32_t optionsSetSyncOffset;
        uint32_t beatMatchCtor;
        uint32_t playerMatcherGetSongMs;
        uint32_t ghUtlInit;        // where GH2 registers its DataFuncs
        uint32_t dataRegisterFunc;
        uint32_t dataReadString;
        uint32_t dataNodeEvaluate; // DataNode::Evaluate
        uint32_t symbolCtor;       // Symbol::Symbol(const char *)
        uint32_t builtinNew;
        uint32_t builtinDelete;
        uint32_t dataNodeGetObj;   // DataNode::GetObj
        uint32_t uiGotoScreen;     // UIManager::GotoScreen
        uint32_t debugModal; // DebugModal(bool &, char *)
        uint32_t abort;
        uint32_t ctlClientPoll;
        uint32_t metaMusicPoll;
        uint32_t scePadRead;
        uint32_t scePadInfoAct;
        uint32_t charHairPoll;
        uint32_t camShotShake;
        uint32_t rndFlareDrawFlare;
        // Data
        uint32_t rndCamCurrent;  // RndCam::sCurrent
        uint32_t defaultMat;     // the RndMat a mesh without one draws with
        uint32_t rndEnvironCurrent; // RndEnviron::sCurrent
        uint32_t synthServerHandler; // the handler CtlServerInit stores
        uint32_t synthServerBuffer;  // its RPC server's receive buffer
        uint32_t theTaskMgr;
        uint32_t theOptions; // Options*
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
        .psRndDrawRect = 0x19b050u,
        .psMeshSync = 0x3d4f08u,
        .psMeshFixVerts = 0x19dbb8u,
        .psMeshDestroy = 0x19dd88u,
        .psMeshDrawShowing = 0x3d88d8u,
        .psMultiMeshDrawShowing = 0x1a2f00u,
        .psParticleSysDrawShowing = 0x1a2c28u,
        .updateRelativeXfm = 0x1cf7b0u,
        .psEnvironSelect = 0x1a2060u,
        .psTexSyncBitmap = 0x1a13a8u,
        .psTexDestroy = 0x1a0f18u,
        .rndTextDrawShowing = 0x1dc380u,
        .worldXfm = 0x3d8ea0u,
        .setWorldXfm = 0x1dd7b8u,
        .playMovie = 0x21bb60u,
        .optionsSyncVideo = 0x10db80u,
        .optionsSetSyncOffset = 0x10ded8u,
        .beatMatchCtor = 0x120f48u,
        .playerMatcherGetSongMs = 0x115738u,
        .ghUtlInit = 0x11fbf0u,
        .dataRegisterFunc = 0x2b2c80u,
        .dataReadString = 0x2b28d0u,
        .dataNodeEvaluate = 0x2b7d38u,
        .symbolCtor = 0x2d3a48u,
        .builtinNew = 0x2cf138u,
        .builtinDelete = 0x2cf160u,
        .dataNodeGetObj = 0x2b7f80u,
        .uiGotoScreen = 0x214d48u,
        .debugModal = 0x105a88u,
        .abort = 0x307b80u,
        .ctlClientPoll = 0x22dc68u,
        .metaMusicPoll = 0x21f180u,
        .scePadRead = 0x2f2c48u,
        .scePadInfoAct = 0x2f2e58u,
        .charHairPoll = 0x176fb8u,
        .camShotShake = 0x262f38u,
        .rndFlareDrawFlare = 0x1f9330u,
        .rndCamCurrent = 0x3de348u,
        .defaultMat = 0x3da4f0u,
        .rndEnvironCurrent = 0x3de358u,
        .synthServerHandler = 0x3de454u,
        .synthServerBuffer = 0x484340u,
        .theTaskMgr = 0x51ee40u,
        .theOptions = 0x3da2e8u,
        .swizzle8 = {0x3de1c8u, 0x3de208u},
        .swizzle4 = {0x3de248u, 0x3de2c8u},
    };
}
