#include "render/look.h"

#include "render/texture_store.h"

#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>

namespace gh2::look
{
    namespace
    {
        // By Mat name, then diffuse payload. Filled wherever a scene file is
        // made.
        std::unordered_map<std::string, std::map<uint32_t, Look>> s_looks;
        std::mutex s_looksMutex;

        // Each Mat's maps once read, held while draws ask for them. Game
        // thread only.
        struct Held
        {
            Look look;
            std::shared_ptr<const Shading> shading;
            uint64_t asked = 0;
        };
        std::unordered_map<std::string, Held> s_held;
        uint64_t s_frame = 0;
        // Frames a Mat's maps outlast the last draw that asked for them.
        constexpr uint64_t kHoldFrames = 600;
        uint32_t s_luminance = 0;

        std::shared_ptr<const TextureData> map(uint32_t id)
        {
            return id != 0u ? textures::get(id) : nullptr;
        }

        bool same(const Look &a, const Look &b)
        {
            return a.normal == b.normal && a.specular == b.specular && a.glow == b.glow && a.ramp == b.ramp &&
                   a.cube == b.cube && a.emissive == b.emissive && a.normalize == b.normalize &&
                   std::memcmp(a.specularColor, b.specularColor, sizeof(a.specularColor)) == 0;
        }
    }

    void add(const std::string &mat, const Look &look)
    {
        const std::lock_guard lock(s_looksMutex);
        s_looks[mat][look.diffuse] = look;
    }

    std::shared_ptr<const Shading> find(const char *mat, uint32_t diffuse)
    {
        Look look;
        {
            const std::lock_guard lock(s_looksMutex);
            const auto byMat = s_looks.find(mat);
            if (byMat == s_looks.end())
                return nullptr;
            // A diffuse texture with no stand-in (no picture of its own) is
            // known by the Mat's name alone when only one scene has it.
            auto byDiffuse = byMat->second.find(diffuse);
            if (byDiffuse == byMat->second.end() && byMat->second.size() == 1u)
                byDiffuse = byMat->second.begin();
            if (byDiffuse == byMat->second.end())
                return nullptr;
            look = byDiffuse->second;
        }
        Held &held = s_held[std::string(mat) + '/' + std::to_string(diffuse)];
        held.asked = s_frame;
        if (!held.shading || !same(held.look, look))
        {
            auto shading = std::make_shared<Shading>();
            shading->normal = map(look.normal);
            shading->specular = map(look.specular);
            shading->glow = map(look.glow);
            shading->ramp = map(look.ramp);
            shading->cube = map(look.cube);
            shading->emissive = look.emissive;
            std::memcpy(shading->specularColor, look.specularColor, sizeof(look.specularColor));
            shading->normalize = look.normalize;
            held.look = look;
            held.shading = std::move(shading);
        }
        return held.shading;
    }

    void endFrame()
    {
        ++s_frame;
        std::erase_if(s_held, [](const auto &entry) { return entry.second.asked + kHoldFrames < s_frame; });
    }

    void setLuminance(uint32_t id) { s_luminance = id; }

    std::shared_ptr<const TextureData> luminance()
    {
        static std::shared_ptr<const TextureData> held;
        if (!held && s_luminance != 0u)
            held = textures::get(s_luminance);
        return held;
    }
}
