#include "content/games.h"

#include "disc/ark.h"
#include "script.h"

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

        script::Node mountedCommand(const script::Call &call)
        {
            return {mounted(call.symbol(1)) ? 1u : 0u, script::kInt};
        }
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
