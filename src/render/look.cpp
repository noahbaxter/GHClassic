#include "render/look.h"

#include "render/texture_store.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
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
        // Turning particle systems' rates, by "system/mat".
        std::map<std::string, std::array<float, 2>> s_spins;

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
        bool s_inVenue = false;
        // Game thread only.
        std::vector<uint32_t> s_boards;
        uint64_t s_boardsAt = 0;
        constexpr size_t kPlayers = 2;
        bool s_star = false;
        bool s_swung = false; // star power has been on since the boards were made
        float s_rest[3][3] = {};

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
            // A Mat with no picture of its own is known by its name alone
            // when only one scene has it. One the 360 gave a picture, asked
            // for without it, is the PS2's Mat of that name: its guitar
            // beside the 360's guitarist, drawn as the PS2 shades it.
            auto byDiffuse = byMat->second.find(diffuse);
            if (byDiffuse == byMat->second.end() && byMat->second.size() == 1u && byMat->second.begin()->first == 0u)
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

    void addSpin(const std::string &system, const std::string &mat, float rpmLow, float rpmHigh)
    {
        const std::lock_guard lock(s_looksMutex);
        s_spins[system + '/' + mat] = {rpmLow, rpmHigh};
    }

    const float *findSpin(const char *system, const char *mat)
    {
        const std::lock_guard lock(s_looksMutex);
        const auto found = s_spins.find(std::string(system) + '/' + mat);
        // Entries are never erased, and a map's stay where they are.
        return found != s_spins.end() ? found->second.data() : nullptr;
    }

    std::string matOf(uint32_t diffuse)
    {
        const std::lock_guard lock(s_looksMutex);
        std::string found;
        for (const auto &[mat, byDiffuse] : s_looks)
            if (byDiffuse.contains(diffuse))
            {
                if (!found.empty())
                    return {};
                found = mat;
            }
        return found;
    }

    void addBoard(uint32_t mat)
    {
        if (s_frame > s_boardsAt + kHoldFrames)
            s_boards.clear();
        s_boardsAt = s_frame;
        // A new board is a new song's, whose star power is off; a song
        // restarted at once adds its boards to the last's, so only as many
        // as there are players are kept.
        s_star = s_swung = false;
        std::erase(s_boards, mat);
        s_boards.push_back(mat);
        if (s_boards.size() > kPlayers)
            s_boards.erase(s_boards.begin());
    }

    void setStarPower(bool on)
    {
        s_star = on;
        s_swung = s_swung || on;
    }

    void restBoard(Environ &board)
    {
        if (s_star)
            return;
        if (s_swung)
            std::memcpy(board.toLight, s_rest, sizeof(s_rest));
        else
            std::memcpy(s_rest, board.toLight, sizeof(s_rest));
    }

    bool isBoard(uint32_t mat) { return std::find(s_boards.begin(), s_boards.end(), mat) != s_boards.end(); }
    bool hasBoard() { return !s_boards.empty(); }

    bool trackFades()
    {
        return settings::hd(settings::kLookHighway) && settings::hd(settings::kLookLighting);
    }

    void endFrame()
    {
        ++s_frame;
        std::erase_if(s_held, [](const auto &entry) { return entry.second.asked + kHoldFrames < s_frame; });
    }

    void setInVenue(bool drawn) { s_inVenue = drawn; }
    bool inVenue() { return s_inVenue; }

    void setLuminance(uint32_t id) { s_luminance = id; }

    std::shared_ptr<const TextureData> luminance()
    {
        // Read once: one that cannot be read is not asked for every frame.
        static std::shared_ptr<const TextureData> held;
        static bool asked = false;
        if (!asked && s_luminance != 0u)
        {
            asked = true;
            held = textures::get(s_luminance);
        }
        return held;
    }
}
