#pragma once

#include "render/scene.h"

#include <cstdint>
#include <memory>

// A mesh's geometry read back out of the packet PsMesh::Sync made of it,
// for memory this run did not sync (dev/transplant.h).
namespace gh2
{
    // Null when the mesh has no packet or it does not read as one.
    std::shared_ptr<const MeshData> decodeMeshPacket(uint8_t *rdram, uint32_t mesh);
}
