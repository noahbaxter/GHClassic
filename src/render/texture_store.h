#pragma once

// Texture pixels kept on the host, for pictures larger than the guest has
// room for. A payload is where a picture is on a disc. The guest loads a
// stand-in bitmap the size its own texture was, whose first pixel bytes
// name the payload, and the payload is read and decoded when a draw first
// samples it (texture_capture.cpp).

#include "render/scene.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gh2::textures
{
    // A Tex object of a scene file in a disc's archive, or with no `tex` a
    // bitmap file there.
    struct Source
    {
        size_t disc = 0;
        std::string path;
        std::string tex;
    };

    // A source's disc for a picture bundled with the game: its path is the
    // name addBundled gave it.
    constexpr size_t kBundled = static_cast<size_t>(-1);

    // A picture bundled with the game, as a 360 bitmap, kept where `data`
    // is for the run. Any thread, before a source names it.
    void addBundled(const std::string &name, const uint8_t *data, size_t size);

    // Its payload's id, the same for the same source, in any run that
    // mounts the same discs. Nothing is read. Any thread.
    uint32_t add(const Source &source);

    // That payload's pixels changed by `edit` each time they are read: a
    // font sheet given glyphs its disc lacks (gh2x/textures.cpp). Any thread.
    void addEdit(uint32_t id, std::function<void(TextureData &)> edit);

    // Those payloads read on the worker's thread, ahead of the draw that
    // will first ask for them: a scene's, as the game loads the scene. All
    // of them are held while the guest keeps a stand-in for any. Game thread
    // only.
    void prefetch(const std::vector<uint32_t> &ids);

    // A guest Tex has synced as a stand-in for that payload, or has let it
    // go (texture_capture.cpp).
    void standInMade(uint32_t id);
    void standInGone(uint32_t id);

    // The bytes a stand-in's pixels start with: a magic word and the id.
    constexpr size_t kNameBytes = 8u;

    // A bitmap file naming that payload: width by height in the smallest
    // form RndBitmap::Load (0x1b10d0) and PsTex::SyncBitmap take, 4 bits a
    // pixel in the PS2 disc's own order, no mips. Empty when its pixels
    // have no room for the name.
    std::vector<uint8_t> standIn(uint32_t id, uint32_t width, uint32_t height);

    // The payload a bitmap's first pixel bytes name, if it is a stand-in.
    std::optional<uint32_t> named(const uint8_t *pixels, size_t size);

    // Its pixels, every level its disc stores, read now unless something
    // still holds them. Null, with the reason printed, if they cannot be.
    // Game thread only.
    std::shared_ptr<const TextureData> get(uint32_t id);

    // The scene file payloads were last read from is let go.
    void endFrame();

    // What get() has held the game thread up for since the run began: the
    // payloads it read itself, and its time reading them or waiting on the
    // worker's. Any thread.
    struct Reads
    {
        uint64_t count = 0;
        uint64_t ns = 0;
    };
    Reads reads();
}
