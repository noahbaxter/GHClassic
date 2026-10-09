#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Native copies of what the engine draws. Built on the game thread from guest
// memory and never changed after, so the host can hold them across frames
// without ever reading the guest.
namespace gh2
{
    // A vert's float as VU1 takes it. VU1 has no NaN or infinity: an exponent
    // of 255 is one more power of two, so such a float is a very large
    // number of its sign (main_hall.5.mesh in big has seven such normals),
    // and a lit colour from it clamps to 0 or 1 where a NaN would blank the
    // triangle. Kept that way here, 28 powers down so sums of it stay finite.
    inline float vuFloat(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        if ((bits & 0x7f800000u) != 0x7f800000u)
            return value;
        bits = (bits & 0x807fffffu) | (227u << 23);
        std::memcpy(&value, &bits, sizeof(bits));
        return value;
    }

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

    // A texture as RGBA8. Alpha 255 is the GS's 0x80, which it
    // blends as 1.0; PS2 alpha above 0x80 clamps.
    struct TextureData
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> rgba;
        // The game's own smaller levels, each half the one before.
        std::vector<std::vector<uint8_t>> mips;
        // A font's glyphs, drawn near their own size: generated levels would
        // only blur them.
        bool text = false;
    };

    // One material pass as the engine held it at draw time.
    struct Material
    {
        uint32_t blend = 1;  // milo::mat::Blend
        uint32_t zMode = 1;  // milo::mat::ZMode
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        bool intensify = false;
        bool alphaCut = false;
        bool alphaWrite = false;    // else every pixel it draws gets the frame buffer's alpha bit
        bool destAlphaTest = false; // drawn only where that bit is clear
        bool texWrap = true;
        bool useEnviron = false;
        bool prelit = false;
        bool vertDyn = false; // lights scaled by the vertex colour: GH1's, which GH2's loader drops
        bool highlight = false; // GS HIGHLIGHT texturing, PsMat +0x130 == 2
        bool decal = false;     // GS DECAL texturing: the texel as it is. GH1's alone
        // Sampled as the mean of four texels, color[0] and [1] apart in u and
        // v (addDepthOfField). The colour is then the vertex's alone.
        bool spread = false;
        std::shared_ptr<const TextureData> texture; // null for none
        uint32_t renderTarget = 0; // the rendered RndTex sampled in place of texture, 0 for none
        uint32_t texGen = 0; // milo::mat::TexGen
        // uv' = u * uvXfm[0..1] + v * uvXfm[2..3] + uvXfm[4..5]
        float uvXfm[6] = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        // The environ tex gen's 3x3 (rows), which the reflection is taken
        // through: tex_xfm's rotation transposed, then y and z swapped.
        float envRows[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}};
        // The projected tex gen's 3x3 (rows) and offset, which take a world
        // position to its uv: tex_xfm inverted, then y and z swapped.
        float projRows[4][3] = {};
        // The sphere tex gen's 3x3 (rows) and offset, which take a normal to
        // its uv. Set for a mesh's pass only (readSphereRows).
        bool sphere = false;
        float sphereRows[4][3] = {};
    };

    // What the current environ gives VU1's lighting programs, as
    // PsEnviron::Select uploads it (qw681..688).
    struct Environ
    {
        enum Kind : uint32_t
        {
            kAmbient,     // no lights: program 0x7c5
            kDirectional, // up to three directional lights: program 0x6ec
            kPoint,       // one point light with a range: program 0x436
        };
        uint32_t kind = kAmbient;
        float ambient[3] = {0.0f, 0.0f, 0.0f};
        uint32_t lightCount = 0;
        float color[3][3] = {};   // rgb; VU1 gets alpha 0. A point light's is color[0]
        float toLight[3][3] = {}; // world, unit length: each light's -y
        float position[3] = {};   // the point light's world position
        float range = 0.0f;       // the point light's range
    };

    struct DrawCall
    {
        std::shared_ptr<const MeshData> mesh;
        Matrix world{};      // identity when skinned
        bool screen = false; // verts already in clip space (PsRnd::DrawRect): no camera
        uint32_t camera = 0; // index into Frame::cameras
        Material material;
        // A skinned vert is sum over b of weight[b] * (pos * bones[b]), its
        // four colour floats being the weights.
        bool skinned = false;
        // The bones of the skin program the mesh runs (UpdateFacePacket
        // 0x3d484c). With two or more it leaves the skinned position and
        // normal in the vert for lighting and tex gen, which then take them
        // through an identity lightWorld. One bone is left as is.
        uint32_t skinBones = 1;
        std::array<Matrix, 4> bones{};
        Environ environment; // not environ, a macro in mingw's stdlib.h
        // Takes normals to world space for lighting (qw676..678): the world
        // transform, or for a skinned mesh the palette's fifth matrix.
        Matrix lightWorld{};
        // The mesh's name, only while a draw dump is asked for.
        std::string name;
    };
}
