#include "content/games.h"

#include "disc/ark.h"
#include "disc/volume.h"
#include "script.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>

namespace gh2::games
{
    namespace
    {
        struct Release
        {
            const char *game;
            const char *serial; // ark::discWithSerial's
        };

        // Every supported release, from config/executables.toml (CMake).
        constexpr Release kReleases[] = {
#include "releases.inc"
        };

        // tools/ghc.py's content_games gives a dev run the same order.
        constexpr const char *kOrder[] = {"gh80s", "gh1", "gh2x"};
        constexpr const char *kEngine = "gh2";

        script::Node mountedCommand(const script::Call &call)
        {
            return {mounted(call.symbol(1)) ? 1u : 0u, script::kInt};
        }
    }

    void mount(const std::vector<std::filesystem::path> &images)
    {
        std::map<std::string, std::filesystem::path> found;
        for (const std::filesystem::path &image : images)
        {
            std::string error;
            const std::unique_ptr<Volume> volume = Volume::open(image, error);
            if (!volume)
            {
                std::cerr << "[games] " << image.filename().string() << ": " << error << std::endl;
                continue;
            }
            const std::string serial = volume->serial();
            const char *game = nullptr;
            for (const Release &release : kReleases)
                if (serial == release.serial)
                    game = release.game;
            if (!game)
                std::cerr << "[games] " << image.filename().string() << ": " << serial
                          << " is no release this build takes content from" << std::endl;
            else if (std::string(game) != kEngine)
                found.emplace(game, image);
        }
        std::vector<std::string> order(std::begin(kOrder), std::end(kOrder));
        for (const auto &[game, image] : found)
            if (std::find(order.begin(), order.end(), game) == order.end())
                order.push_back(game);
        for (const std::string &game : order)
            if (const auto it = found.find(game); it != found.end())
                ark::addDisc(it->second);
    }

    std::optional<size_t> disc(const std::string &game)
    {
        for (const Release &release : kReleases)
            if (game == release.game)
                if (const auto found = ark::discWithSerial(release.serial))
                    return found;
        return std::nullopt;
    }

    bool mounted(const std::string &game) { return disc(game).has_value(); }

    void install(PS2Runtime &, const Addresses &)
    {
        script::addCommand("mounted", mountedCommand);
    }
}
