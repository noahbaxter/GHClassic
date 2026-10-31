#pragma once

#include "addresses.h"
#include "render/scene.h"

#include <cstdint>
#include <memory>
#include <vector>

class PS2Runtime;

namespace gh2
{
    void installNativeMesh(PS2Runtime &runtime, const Addresses &addresses);

    // More geometry drawn with a mesh, under its transform and material:
    // what the rider adds to `out` for the mesh, given the geometry it last
    // synced.
    using MeshRider = void (*)(uint8_t *rdram, uint32_t mesh, const MeshData &geometry,
                               std::vector<std::shared_ptr<const MeshData>> &out);
    void setMeshRider(MeshRider rider);

    // A Transform in guest memory: three 16-byte rows, then the position.
    Matrix readTransform(uint8_t *rdram, uint32_t address);

    // The camera current when a draw is made, as an index into the frame
    // being built, reusing the last entry when nothing about it has changed.
    uint32_t currentCamera(uint8_t *rdram);
}
