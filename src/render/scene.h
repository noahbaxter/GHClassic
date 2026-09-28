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

    // A texture's top level as RGBA8. Alpha 255 is the GS's 0x80, which it
    // blends as 1.0; PS2 alpha above 0x80 clamps.
    struct TextureData
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    // One material pass as the engine held it at draw time.
    struct Material
    {
        uint32_t blend = 1;  // milo::mat::Blend
        uint32_t zMode = 1;  // milo::mat::ZMode
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool intensify = false;
        bool alphaCut = false;
        bool texWrap = true;
        bool useEnviron = false;
        bool prelit = false;
        std::shared_ptr<const TextureData> texture; // null for none
        uint32_t texGen = 0; // milo::mat::TexGen
        // uv' = u * uvXfm[0..1] + v * uvXfm[2..3] + uvXfm[4..5]
        float uvXfm[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    };

    // What the current environ gives VU1's lighting programs, as
    // PsEnviron::Select uploads it (qw681..688).
    struct Environ
    {
        enum Kind : uint32_t
        {
            kAmbient,     // no lights: program 0x7c5
            kDirectional, // up to three directional lights: program 0x6ec
            kPoint,       // a point light: program 0x436, not implemented
        };
        uint32_t kind = kAmbient;
        float ambient[3] = {0.0f, 0.0f, 0.0f};
        uint32_t lightCount = 0;
        float color[3][3] = {};   // rgb; VU1 gets alpha 0
        float toLight[3][3] = {}; // world, unit length: each light's -y
    };

    struct DrawCall
    {
        std::shared_ptr<const MeshData> mesh;
        Matrix world{};
        uint32_t camera = 0; // index into Frame::cameras
        Material material;
        Environ environ;
        // Takes normals to world space for lighting (qw676..678): the world
        // transform.
        Matrix lightWorld{};
    };
}
