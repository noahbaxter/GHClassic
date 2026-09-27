#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Native copies of what the engine draws. Built on the game thread from guest
// memory and never changed after, so the host can hold them across frames
// without ever reading the guest.
namespace gh2
{
    struct Vertex
    {
        float pos[3];
        float normal[3];
        float color[4];
        float uv[2];
    };

    struct MeshData
    {
        std::vector<Vertex> verts;
        std::vector<uint16_t> indices; // triangle list
    };

    using Matrix = std::array<float, 16>; // row-major, row vectors

    // One material pass as the engine held it at draw time.
    struct Material
    {
        uint32_t blend = 1;  // milo::mat::Blend
        uint32_t zMode = 1;  // milo::mat::ZMode
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool intensify = false;
        bool alphaCut = false;
        bool texWrap = true;
        uint32_t texture = 0; // the RndTex's guest address, 0 for none
    };

    struct DrawCall
    {
        std::shared_ptr<const MeshData> mesh;
        Matrix world{};
        uint32_t camera = 0; // index into Frame::cameras
        Material material;
    };
}
