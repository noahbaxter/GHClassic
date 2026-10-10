#include "content/quickplay.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "content/outfits.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "guest.h"
#include "script.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace gh2::quickplay
{
    namespace
    {
        const std::vector<std::string> kPicked = {"characters", "guitars"};

        bool s_quickplay = false;
        std::string s_borrowedFor; // the menus' game whose arrays are GH2's for quickplay, or none
        std::vector<std::string> s_lent;

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

        // Quickplay's characters and guitars are GH2's, every game's outfits
        // among them, whatever game's menus these are; leaving, the menus'
        // own again. The 80s disc has its own outfits at GH2's paths, so
        // GH2's are lent from the game disc while it is in front.
        void enter(const script::Call &call)
        {
            s_quickplay = true;
            const std::string home = campaigns::active();
            if (home == "gh2" || !s_borrowedFor.empty() || !campaigns::configFrom(call, "gh2", kPicked))
                return;
            s_borrowedFor = home;
            outfits::addTo(call, "gh2");
            std::vector<std::string> names = campaigns::outfits("gh2");
            for (const std::string &outfit : outfits::all())
                names.push_back(outfit);
            outfits::preview(call, names);
            if (home == "gh80s")
                for (const std::string &outfit : campaigns::outfits("gh2"))
                    for (const std::string &prefix : {"char/" + outfit + "/", "track/surfaces/gen/" + outfit + "_keep",
                                                      "ui/image/og/gen/photo_" + outfit})
                    {
                        ark::lend(prefix, std::nullopt);
                        s_lent.push_back(prefix);
                    }
            std::cerr << "[quickplay] " << home << "'s characters and guitars are GH2's in quickplay" << std::endl;
        }

        void leave(const script::Call &call)
        {
            s_quickplay = false;
            if (s_borrowedFor.empty())
                return;
            for (const std::string &prefix : s_lent)
                ark::unlend(prefix);
            s_lent.clear();
            // A switch since has put back its own.
            if (s_borrowedFor == campaigns::active() && campaigns::configFrom(call, s_borrowedFor, kPicked))
                outfits::addTo(call, s_borrowedFor);
            s_borrowedFor.clear();
        }

        // Unlocked in quickplay where any game's career has it to use: an
        // added outfit by its own game's name for it.
        bool anywhere(const std::string &item)
        {
            if (const auto own = outfits::ownOf(item))
                return campaigns::unlockedIn(own->first, own->second) || campaigns::unlockedIn(own->first, item);
            for (const std::string &game : campaigns::games())
                if (campaigns::unlockedIn(game, item))
                    return true;
            return false;
        }

        // Campaign::IsItemUnlocked(Symbol) (0x12d490) and (Symbol,
        // Difficulty) (0x12d5a0), which the pickers ask outside a career:
        // an item any band has unlocked in the game played, or in quickplay
        // in any game.
        PS2Runtime::RecompiledFunction s_isItemUnlocked = nullptr, s_isItemUnlockedOn = nullptr;
        const Addresses *s_addresses = nullptr;
        template <bool kOn>
        void isItemUnlocked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const std::string item = reinterpret_cast<const char *>(getMemPtr(rdram, GPR_U32(ctx, 5)));
            uint32_t unlocked =
                kOn ? static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->campaignIsItemUnlockedOn,
                                                                       {GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6)},
                                                                       s_isItemUnlockedOn))
                    : static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->campaignIsItemUnlocked,
                                                                       {GPR_U32(ctx, 4), GPR_U32(ctx, 5)},
                                                                       s_isItemUnlocked));
            if ((unlocked & 0xffu) == 0u && s_quickplay && anywhere(item))
                unlocked = 1u;
            SET_GPR_U32(ctx, 2, unlocked);
            ctx->pc = returnTo;
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
                // added with (content/outfits.h); GH2's, and the game played's
                // outside quickplay, their own.
                const std::string game = call.symbol(2), own = bootOutfit(game);
                const bool home = game == (s_borrowedFor.empty() ? campaigns::active() : "gh2");
                const std::string outfit = home ? own : outfits::named(game, own).value_or(own);
                return {script::symbol(call.rdram, call.ctx, call.runtime, outfit), script::kSymbol};
            }
            if (op == "quickplay" && call.size() > 2)
            {
                if (call.number(2) != 0)
                    enter(call);
                else
                    leave(call);
                return {s_quickplay ? 1u : 0u, script::kInt};
            }
            std::cerr << "[quickplay] usage: {quickplay song_defaults [<0|1>]}, {quickplay outfit <game>}, "
                         "{quickplay quickplay <0|1>}"
                      << std::endl;
            return {};
        }
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("quickplay", quickplayCommand);
        s_isItemUnlocked = runtime.lookupFunction(addresses.campaignIsItemUnlocked);
        runtime.replaceFunction(addresses.campaignIsItemUnlocked, &isItemUnlocked<false>);
        s_isItemUnlockedOn = runtime.lookupFunction(addresses.campaignIsItemUnlockedOn);
        runtime.replaceFunction(addresses.campaignIsItemUnlockedOn, &isItemUnlocked<true>);
    }
}
