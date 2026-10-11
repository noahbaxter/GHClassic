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

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace gh2::quickplay
{
    namespace
    {
        std::map<std::string, std::string> s_picked; // quickplay's last picks, by what they are
        std::map<std::string, std::string> s_looked; // each character's outfit last looked at

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

        // Unlocked outside a career where any game's career has it to use:
        // an added outfit by its own game's name for it.
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
        // Difficulty) (0x12d5a0), which the pickers ask: an item any band
        // has unlocked in the game played, or outside a career in any game.
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
            if ((unlocked & 0xffu) == 0u && !campaigns::inCareer(rdram) && anywhere(item))
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
            // A choice setting by its word, kept in settings.ini; the word.
            const auto choice = [&](settings::Key key, std::vector<std::string> words) -> script::Node
            {
                if (call.size() > 2)
                    if (const auto it = std::find(words.begin(), words.end(), call.symbol(2)); it != words.end())
                        settings::set(key, static_cast<int>(it - words.begin()));
                const size_t at = static_cast<size_t>(settings::get(key));
                return {script::symbol(call.rdram, call.ctx, call.runtime, at < words.size() ? words[at] : ""),
                        script::kSymbol};
            };
            if (op == "in_song_ui")
                return choice(settings::kInSongUi, {"venue", "gh1", "gh2", "gh80s"});
            if (op == "gems")
                return choice(settings::kGems, {"auto", "gh1", "gh2"});
            if (op == "menu_music")
                return choice(settings::kMenuMusic, {"theme", "gh1", "gh2", "gh80s", "all"});
            if (op == "menu_music_order")
                return choice(settings::kMenuMusicOrder, {"playlist", "loop", "shuffle"});
            if (op == "outfit" && call.size() > 2)
            {
                // Another game's outfit is in the config under the name it was
                // added with (content/outfits.h); the game played's, GH2's
                // outside a career, its own.
                const std::string game = call.symbol(2), own = bootOutfit(game);
                const std::string outfit = game == campaigns::active() ? own : outfits::named(game, own).value_or(own);
                return {script::symbol(call.rdram, call.ctx, call.runtime, outfit), script::kSymbol};
            }
            if (op == "pick" && call.size() > 2)
            {
                if (call.size() > 3)
                    s_picked[call.symbol(2)] = call.symbol(3);
                const auto it = s_picked.find(call.symbol(2));
                return {script::symbol(call.rdram, call.ctx, call.runtime, it != s_picked.end() ? it->second : ""),
                        script::kSymbol};
            }
            if (op == "picked" && call.size() > 2)
                return {s_picked.count(call.symbol(2)) != 0u ? 1u : 0u, script::kInt};
            // The outfit last looked at on a character, which hovering it
            // shows again (sel_character.dta hover_outfit); 0 if none.
            if (op == "look" && call.size() > 2)
            {
                if (call.size() > 3)
                    s_looked[call.symbol(2)] = call.symbol(3);
                const auto it = s_looked.find(call.symbol(2));
                if (it == s_looked.end())
                    return {0u, script::kInt};
                return {script::symbol(call.rdram, call.ctx, call.runtime, it->second), script::kSymbol};
            }
            std::cerr << "[quickplay] usage: {quickplay song_defaults [<0|1>]}, {quickplay outfit <game>}, "
                         "{quickplay pick|picked <what>}, {quickplay look <character> [<outfit>]}, "
                         "{quickplay in_song_ui [venue|gh1|gh2|gh80s]}, {quickplay gems [auto|gh1|gh2]}, "
                         "{quickplay menu_music [theme|gh1|gh2|gh80s|all]}, "
                         "{quickplay menu_music_order [playlist|loop|shuffle]}"
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
