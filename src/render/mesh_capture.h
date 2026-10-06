#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <memory>

class PS2Runtime;

namespace gh2
{
    // The geometry a mesh object last synced, or null. Game thread only.
    std::shared_ptr<const MeshData> capturedMesh(uint8_t *rdram, uint32_t mesh);

    // From here on guest memory is another run's (dev/transplant.h), whose
    // syncs were never seen: a mesh's geometry is read out of its packet.
    void readMeshesFromPackets();

    void installMeshCapture(PS2Runtime &runtime, const Addresses &addresses);
}
