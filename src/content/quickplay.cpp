#include "content/quickplay.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "content/outfits.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "script.h"
#include "settings/settings.h"

#include <iostream>
#include <map>
#include <string>

namespace gh2::quickplay
{
    namespace
    {
        // The first {game set_character <outfit> ...} under `node`.
        const dtb::Node *setCharacter(const dtb::Node &node)
        {
            if (node.nodes.size() > 2u && node.nodes[0].text == "game" && node.nodes[1].text == "set_character" &&
                node.nodes[2].type == dtb::kSymbol)
                return &node.nodes[2];
            for (const dtb::Node &child : node.nodes)
                if (const dtb::Node *found = setCharacter(child))
                    return found;
            return nullptr;
        }

        // That game's own name for the outfit it boots with, off its disc:
        // punk1 in GH2 and the 80s, metal in GH1, whose UI is under ghui/.
        // GH2's where the disc gives none.
        std::string bootOutfit(const std::string &game)
        {
            static std::map<std::string, std::string> s_found;
            if (const auto it = s_found.find(game); it != s_found.end())
                return it->second;
            const size_t disc = games::disc(game).value_or(0u);
            dtb::Macros macros;
            const auto main = dtb::read(game == "gh1" ? "ghui/main.dta" : "ui/main.dta", macros,
                                        [disc](const std::string &path) { return ark::readFile(disc, path); });
            const dtb::Node *outfit = main ? setCharacter(*main) : nullptr;
            return s_found[game] = outfit ? outfit->text : "punk1";
        }

        script::Node quickplayCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "song_defaults")
            {
                if (call.size() > 2)
                    settings::set(settings::kSongDefaults, call.number(2) != 0 ? 1 : 0);
                return {settings::get(settings::kSongDefaults) ? 1u : 0u, script::kInt};
            }
            if (op == "outfit" && call.size() > 2)
            {
                // Another game's outfit is in the config under the name it was
                // added with (content/outfits.h); the game played's, its own.
                const std::string game = call.symbol(2), own = bootOutfit(game);
                const std::string outfit =
                    game == campaigns::active() ? own : outfits::named(game, own).value_or(own);
                return {script::symbol(call.rdram, call.ctx, call.runtime, outfit), script::kSymbol};
            }
            std::cerr << "[quickplay] usage: {quickplay song_defaults [<0|1>]}, {quickplay outfit <game>}" << std::endl;
            return {};
        }
    }

    void install(PS2Runtime &, const Addresses &)
    {
        script::addCommand("quickplay", quickplayCommand);
    }
}
