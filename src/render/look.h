#pragma once

// A 360 material's shading beyond what the PS2's Mat holds: its normal,
// specular, glow, diffuse ramp and cube maps, kept on the host as texture
// payloads (render/texture_store.h), and the values only the 360's shader
// reads. Filled as a converted scene loads (gh2x/models.cpp) and found at
// draw time (render/native_mat.cpp) by the Mat's name and the payload its
// diffuse texture stands in for, which names its 360 file: a scene's own
// directory has no name when a proxy loads it (battle's geometry).

#include "render/scene.h"

#include <cstdint>
#include <memory>
#include <string>

namespace gh2::look
{
    struct Look
    {
        // Payload ids, 0 for none. The diffuse's is the key's, not a map.
        uint32_t diffuse = 0;
        uint32_t normal = 0;
        uint32_t specular = 0;
        uint32_t glow = 0;
        uint32_t ramp = 0;
        uint32_t cube = 0;
        float emissive = 1.0f;
        float specularColor[4] = {}; // rgb without a specular map, then the power
        bool normalize = false;
    };

    void add(const std::string &mat, const Look &look);

    // The shading of the Mat of that name whose diffuse texture stands in
    // for that payload (0 for none), its maps read: none when no scene
    // registered one. Game thread only.
    std::shared_ptr<const Shading> find(const char *mat, uint32_t diffuse);

    // A 360 ParticleSys whose particles turn, by its name and its Mat's
    // (two venues share a system's name), and the turns a minute each
    // particle takes, somewhere between the two.
    void addSpin(const std::string &system, const std::string &mat, float rpmLow, float rpmHigh);
    // That system's two rates, null when its particles do not turn.
    const float *findSpin(const char *system, const char *mat);

    // Maps no draw has asked for in a while are let go.
    void endFrame();

    // The 360's luminance map, which its post stage scales the world's
    // colour by (render/frame.h), as a payload: 0 for none.
    void setLuminance(uint32_t id);
    // Its pixels. Game thread only.
    std::shared_ptr<const TextureData> luminance();
}
