#include "content/quickplay.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "content/outfits.h"
#include "content/setlists.h"
#include "custom/cast.h"
#include "custom/songs.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh2x/models.h"
#include "guest.h"
#include "hook.h"
#include "script.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <atomic>
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
        // A guitar or finish outside a career is there by the careers alone
        // (campaigns::guitarUnlocked) or the unlock-all cheat (+0x80): the
        // game counts every one no store sells a band's. In a career
        // another game's finish, lent to the config, is nobody's.
        constexpr uint32_t kAllAccess = 0x80u;
        PS2Runtime::RecompiledFunction s_isItemUnlocked = nullptr, s_isItemUnlockedOn = nullptr;
        const Addresses *s_addresses = nullptr;
        template <bool kOn>
        void isItemUnlocked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const std::string item = reinterpret_cast<const char *>(getMemPtr(rdram, GPR_U32(ctx, 5)));
            const bool allAccess = load<uint32_t>(rdram, GPR_U32(ctx, 4) + kAllAccess) != 0u;
            uint32_t unlocked =
                kOn ? static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->campaignIsItemUnlockedOn,
                                                                       {GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6)},
                                                                       s_isItemUnlockedOn))
                    : static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->campaignIsItemUnlocked,
                                                                       {GPR_U32(ctx, 4), GPR_U32(ctx, 5)},
                                                                       s_isItemUnlocked));
            if (campaigns::inCareer(rdram))
            {
                if (campaigns::lentFinish(item))
                    unlocked = 0u;
            }
            else if (const auto guitar = campaigns::guitarUnlocked(item))
                unlocked = allAccess || *guitar ? 1u : 0u;
            else if ((unlocked & 0xffu) == 0u && anywhere(item))
                unlocked = 1u;
            // Pro Face-Off is there from the start, where retail keeps it
            // for a career won (campaign.dta's unlock_game_modes): so no
            // career's end has its letter to show either (endgame.dta).
            if (item == "multi_fo")
                unlocked = 1u;
            SET_GPR_U32(ctx, 2, unlocked);
            ctx->pc = returnTo;
        }

        // Campaign::IsVenueUnlockedQuickplay(Symbol) (0x12de88), which
        // set_quickplay asks of a song's venue: open under the unlock-all
        // cheat too, which retail leaves to a band's progress alone. With it
        // on, quickplay has everything.
        PS2Runtime::RecompiledFunction s_isVenueUnlocked = nullptr;
        void isVenueUnlocked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            uint32_t unlocked = 1u;
            if (load<uint32_t>(rdram, GPR_U32(ctx, 4) + kAllAccess) == 0u)
                unlocked = static_cast<uint32_t>(runtime->callGuestFunction(
                    rdram, ctx, s_addresses->campaignIsVenueUnlockedQuickplay, {GPR_U32(ctx, 4), GPR_U32(ctx, 5)},
                    s_isVenueUnlocked));
            SET_GPR_U32(ctx, 2, unlocked);
            ctx->pc = returnTo;
        }

        // SelectGuitarProvider::InitData (0x119060) lists a player's
        // guitars: for the Grim Ripper (+0x3c) the scythe alone, else
        // quickplay's (Campaign::GetQuickplayGuitars, 0x12df48: config's of
        // type guitar that are unlocked) or the career's. Outside a career
        // the config has every game's finishes, the scythe is a guitar like
        // any other, and the Grim Ripper has them all. The list is
        // quickplay's (+0x40) there whoever asks: the career's is the
        // band's items, which a guitar lent whole is not one of.
        constexpr uint32_t kProviderCharacter = 0x3cu, kProviderQuickplay = 0x40u;
        PS2Runtime::RecompiledFunction s_guitarsInitData = nullptr;
        std::string configGuitars(uint8_t *rdram, bool free)
        {
            return std::string("{if {find_exists $syscfg guitars scythe type} {set_elem {find $syscfg guitars scythe type} 1 ") +
                   (free ? "guitar" : "grim") + "}}\n" + (free ? campaigns::lendFinishes() : std::string()) +
                   campaigns::ownFinishes(rdram);
        }
        void guitarsInitData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31), provider = GPR_U32(ctx, 4), dir = GPR_U32(ctx, 5);
            const bool free = !campaigns::inCareer(rdram);
            script::run(rdram, ctx, runtime, configGuitars(rdram, free));
            const uint32_t character = load<uint32_t>(rdram, provider + kProviderCharacter);
            const bool grim = free && character != 0u &&
                              std::string(reinterpret_cast<const char *>(getMemPtr(rdram, character))) == "grim";
            if (grim)
                store<uint32_t>(rdram, provider + kProviderCharacter, script::symbol(rdram, ctx, runtime, ""));
            const uint32_t quickplay = load<uint32_t>(rdram, provider + kProviderQuickplay);
            if (free)
                store<uint32_t>(rdram, provider + kProviderQuickplay, 1u);
            runtime->callGuestFunction(rdram, ctx, s_addresses->selectGuitarProviderInitData, {provider, dir}, s_guitarsInitData);
            store<uint32_t>(rdram, provider + kProviderQuickplay, quickplay);
            if (grim)
                store<uint32_t>(rdram, provider + kProviderCharacter, character);
            ctx->pc = returnTo;
        }

        // A CharsysPanel keeps the models it has loaded (+0x5c, a vector of
        // CacheModel, 0x1c each) and loads the one on show again once it is
        // unloaded (PollCharLoading, 0x142ea8): from the other disc, where
        // which one serves it has changed. The panels are made once, with
        // the UI.
        constexpr uint32_t kPanelModels = 0x5cu, kCacheModelBytes = 0x1cu;
        std::vector<uint32_t> s_charPanels;
        struct ShowCharTag;
        void onShowChar(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            if (std::find(s_charPanels.begin(), s_charPanels.end(), panel) == s_charPanels.end())
                s_charPanels.push_back(panel);
        }
        void reloadCharacters(const script::Call &call)
        {
            for (const uint32_t panel : s_charPanels)
            {
                const uint32_t end = load<uint32_t>(call.rdram, panel + kPanelModels + 4u);
                for (uint32_t model = load<uint32_t>(call.rdram, panel + kPanelModels); model < end;
                     model += kCacheModelBytes)
                    if (call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->cacheModelIsLoading, {model}) == 0u)
                        call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->cacheModelUnload, {model});
            }
        }

        // GuitarEl::LoadGuitar (0x1470d8) leaves alone a guitar it already
        // shows, by its config entry (+4): forgotten once for each after a
        // change, so the next one shown is loaded whatever it is.
        constexpr uint32_t kGuitarElConfig = 0x4u;
        uint32_t s_guitarsChanged = 0u;
        std::map<uint32_t, uint32_t> s_guitarEls;
        struct LoadGuitarTag;
        void onLoadGuitar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t &seen = s_guitarEls[GPR_U32(ctx, 4)];
            if (seen != s_guitarsChanged)
                store<uint32_t>(rdram, GPR_U32(ctx, 4) + kGuitarElConfig, 0u);
            seen = s_guitarsChanged;
        }
        void reloadGuitars(const script::Call &) { ++s_guitarsChanged; }

        // An outfit to play outside a career, by the live config's name for
        // it, with its character: the config's own and every other game's
        // added (content/outfits.h), open once unlocked, or all of them
        // under the unlock-all cheat, as the pickers have them.
        struct Look
        {
            std::string character, outfit;
            bool open;
        };
        std::vector<Look> looks(bool unlockAll)
        {
            std::vector<Look> out;
            const std::string &activeGame = campaigns::active();
            // The character must be unlocked too: a store sells some (GH1's
            // Izzy, the Grim Ripper) but never their outfits. The Grim Ripper
            // only through his own game's career, as the config has his
            // scythe only then.
            for (const auto &[character, characterOutfits] : campaigns::characters(activeGame))
                for (const std::string &outfit : characterOutfits)
                    out.push_back({character, outfit,
                                   unlockAll || (anywhere(outfit) && (character == "grim" ? campaigns::unlockedIn(activeGame, character)
                                                                                         : anywhere(character)))});
            for (const outfits::Outfit &added : outfits::added())
                if (added.game != activeGame)
                    out.push_back({added.character, added.outfit,
                                   unlockAll || (anywhere(added.outfit) && campaigns::unlockedIn(added.game, added.character))});
            return out;
        }

        bool has(const std::vector<std::string> &list, const std::string &item)
        {
            return std::find(list.begin(), list.end(), item) != list.end();
        }

        // A guitar in one of `from`'s finishes of `guitars` that the song
        // picks, as far along as `at` is (custom/cast.h's pickAlong), if
        // unlocked; else none. With the 360's guitars preferred, only
        // finishes it has a model of.
        std::string openGuitar(const std::vector<campaigns::Finish> &from, const std::vector<std::string> &guitars,
                               uint32_t seed, std::optional<double> at, bool unlockAll)
        {
            const bool hd = settings::hd(settings::kLookGuitars);
            std::vector<const campaigns::Finish *> finishes;
            for (const campaigns::Finish &f : from)
                if (has(guitars, f.guitar) && (!hd || gh2xHasModel("char/og/guitars/gen/" + f.model + ".milo_ps2")))
                    finishes.push_back(&f);
            if (finishes.empty())
                return "";
            const campaigns::Finish &pick = *finishes[custom::pickAlong(seed, custom::kSaltFinish, finishes.size(), at)];
            return unlockAll || campaigns::guitarUnlocked(pick.finish).value_or(false) ? pick.guitar + " " + pick.finish : "";
        }

        // A 360 song (x_<name>) cast by castSong, until the song list or the
        // main menu shows again.
        std::atomic<bool> s_nextGenSong{false};

        // Recasts a song for song defaults after set_quickplay has cast it
        // as songs.dta says: `song` from `game`'s setlist, `retail` its
        // outfit there.
        //
        // A disc song keeps that cast, its guitarist under the name its
        // game's outfit is added as; GH2's bonus songs (PS2 and 360) play the
        // same guitarist's alternate outfit once unlocked (GH1's and the
        // 80s' keep theirs).
        //
        // A custom song plays its guitarist in an outfit and its guitar in a
        // finish, each picked by the song from every game's (custom/cast.h),
        // where unlocked; else songs.dta's. The Grim Ripper plays his own
        // game's scythe.
        //
        // Unlocked: a career has it, or the unlock-all cheat is on. With the
        // 360's guitarists or guitars preferred, picks are only among those
        // it has models of, so none is drawn as the PS2's.
        void castSong(const script::Call &call)
        {
            const std::string song = call.symbol(2), game = call.symbol(3), retail = call.symbol(4);
            s_nextGenSong = song.rfind("gh2x_", 0) == 0u;
            const std::string &activeGame = campaigns::active();
            const auto liveName = [&](const std::string &outfit)
            { return game == activeGame ? outfit : outfits::named(game, outfit).value_or(outfit); };
            const uint32_t campaign = load<uint32_t>(call.rdram, s_addresses->theCampaign);
            const bool unlockAll = campaign != 0u && load<uint32_t>(call.rdram, campaign + kAllAccess) != 0u;
            const std::vector<Look> all = looks(unlockAll);
            const auto isOpen = [&](const std::string &outfit)
            { return std::any_of(all.begin(), all.end(), [&](const Look &look) { return look.open && look.outfit == outfit; }); };
            const bool hdCharacters = settings::hd(settings::kLookCharacters);
            // One of `choices` by `seed` if unlocked, else none.
            const auto pickIfOpen = [&](std::vector<std::string> choices, uint32_t seed) -> std::string
            {
                if (hdCharacters)
                    choices.erase(std::remove_if(choices.begin(), choices.end(),
                                                 [](const std::string &o) { return !gh2xHasModel("char/" + o + "/"); }),
                                  choices.end());
                if (choices.empty())
                    return "";
                const std::string &pick = choices[custom::pickOf(seed, custom::kSaltOutfit, choices.size())];
                return isOpen(pick) ? pick : "";
            };

            std::string outfit = liveName(retail), guitar;
            const auto customSong = custom::catalog().find(song);
            if (customSong)
            {
                const custom::Cast cast = custom::castOf(customSong->tags);
                std::vector<std::string> characterOutfits;
                for (const Look &look : all)
                    if (look.character == cast.character)
                        characterOutfits.push_back(look.outfit);
                if (const std::string pick = pickIfOpen(characterOutfits, cast.seed); !pick.empty())
                    outfit = pick;
                if (cast.character == "grim" && outfit != retail)
                {
                    // Each Grim Ripper his own game's scythe, which comes with him.
                    const auto origin = outfits::ownOf(outfit);
                    guitar = "scythe " + campaigns::scytheOf(origin ? origin->first : activeGame);
                }
                else
                    guitar = openGuitar(campaigns::finishes(), {cast.realGuitar}, cast.seed, std::nullopt, unlockAll);
            }
            else
            {
                if (game == "gh2" && (has(setlists::bonusSongs("gh2"), song) || has(setlists::bonusSongs("gh2x"), song)))
                    for (const auto &[character, characterOutfits] : campaigns::characters("gh2"))
                        if (has(characterOutfits, retail))
                        {
                            std::vector<std::string> alternates;
                            for (const std::string &other : characterOutfits)
                                if (other != retail)
                                    alternates.push_back(liveName(other));
                            if (const std::string pick = pickIfOpen(alternates, custom::seedOf(song)); !pick.empty())
                                outfit = pick;
                        }
                // Ours alone for now (settings.ini, no menu): a disc song's
                // guitar among its own game's real ones, cheapest first, a
                // career song picking as far along as its tier is, a bonus
                // song among all.
                if (settings::get(settings::kDiscSongGuitars))
                {
                    const std::string setlist = setlists::shown();
                    guitar = openGuitar(campaigns::finishesOf(setlist), custom::realGuitars(), custom::seedOf(song),
                                        setlists::careerAt(setlist, song), unlockAll);
                }
            }
            // A disc song's own guitarist plays locked or not, as on its disc
            // (SetQuickplay asks no unlock of one), where the config has
            // them: another game's outfit is in it only once that game's
            // career has it (content/outfits.h). Else, and for a custom
            // song, the one its game boots with.
            if (!isOpen(outfit) && (customSong || outfits::ownOf(outfit)))
                outfit = liveName(bootOutfit(game));

            // Every game's finishes in the config first, as the guitar
            // picker has them (guitarsInitData).
            std::string text = configGuitars(call.rdram, true) + "{game set_character " + outfit + " TRUE}\n";
            if (!guitar.empty())
                text += "{game set_guitar " + guitar + "}\n";
            script::run(call.rdram, call.ctx, call.runtime, text);
        }

        script::Node quickplayCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "cast" && call.size() > 4)
            {
                if (!campaigns::inCareer(call.rdram))
                    castSong(call);
                return {};
            }
            if (op == "next_gen_song" && call.size() > 2)
            {
                s_nextGenSong = call.number(2) != 0;
                return {};
            }
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
            if (op == "menu_music")
                return choice(settings::kMenuMusic, {"theme", "gh1", "gh2", "gh80s", "all"});
            if (op == "menu_music_order")
                return choice(settings::kMenuMusicOrder, {"playlist", "loop", "shuffle"});
            // One that says which disc's models are served: changed, what
            // is on show is loaded again.
            const auto models = [&](settings::Key key, void (*reload)(const script::Call &)) -> script::Node
            {
                const int was = settings::get(key);
                const script::Node now = choice(key, {"sd", "hd"});
                if (settings::get(key) != was)
                    reload(call);
                return now;
            };
            if (op == "characters")
                return models(settings::kCharacters, reloadCharacters);
            if (op == "guitars")
                return models(settings::kGuitars, reloadGuitars);
            if (op == "venues")
                return choice(settings::kVenues, {"sd", "hd"});
            // An outfit's scenes are a folder by its name, a finish's one
            // file by its (outfit ...).
            if (op == "has_hd" && call.size() > 3)
            {
                const std::string path = call.symbol(2) == "guitar" ? "char/og/guitars/gen/" + call.symbol(3) + ".milo_ps2"
                                                                    : "char/" + call.symbol(3) + "/";
                return {gh2xHasModel(path) ? 1u : 0u, script::kInt};
            }
            // One outfit's or finish's look by itself, the 360's or the PS2's
            // whatever the setting says, for one player of two; what is on
            // show of it is loaded again only if that changes it.
            if (op == "model" && call.size() > 2)
            {
                const std::string what = call.symbol(2);
                if (what == "clear")
                {
                    if (clearGh2xLooks())
                    {
                        reloadCharacters(call);
                        reloadGuitars(call);
                    }
                    return {1u, script::kInt};
                }
                if (call.size() > 4)
                {
                    const bool guitar = what == "guitar";
                    const std::string path = guitar ? "char/og/guitars/gen/" + call.symbol(3) + ".milo_ps2"
                                                    : "char/" + call.symbol(3) + "/";
                    const std::string look = call.symbol(4);
                    const bool was = gh2xServes(path);
                    setGh2xLook(path, look == "default" ? std::nullopt : std::optional<bool>(look == "hd"));
                    const bool now = gh2xServes(path);
                    if (now != was)
                        (guitar ? reloadGuitars : reloadCharacters)(call);
                    return {(now ? 1u : 0u) | (now != was ? 2u : 0u), script::kInt};
                }
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
            std::cerr << "[quickplay] usage: {quickplay song_defaults [<0|1>]}, "
                         "{quickplay cast <song> <game> <outfit>}, {quickplay next_gen_song 0}, "
                         "{quickplay pick|picked <what>}, {quickplay look <character> [<outfit>]}, "
                         "{quickplay in_song_ui [venue|gh1|gh2|gh80s]},"
                         "{quickplay menu_music [theme|gh1|gh2|gh80s|all]}, "
                         "{quickplay menu_music_order [playlist|loop|shuffle]}, "
                         "{quickplay characters|guitars|venues [sd|hd]}, {quickplay has_hd outfit|guitar <name>}"
                      << std::endl;
            return {};
        }
    }

    bool nextGenSong() { return s_nextGenSong; }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("quickplay", quickplayCommand);
        s_isItemUnlocked = runtime.lookupFunction(addresses.campaignIsItemUnlocked);
        runtime.replaceFunction(addresses.campaignIsItemUnlocked, &isItemUnlocked<false>);
        s_isItemUnlockedOn = runtime.lookupFunction(addresses.campaignIsItemUnlockedOn);
        runtime.replaceFunction(addresses.campaignIsItemUnlockedOn, &isItemUnlocked<true>);
        s_isVenueUnlocked = runtime.lookupFunction(addresses.campaignIsVenueUnlockedQuickplay);
        runtime.replaceFunction(addresses.campaignIsVenueUnlockedQuickplay, &isVenueUnlocked);
        s_guitarsInitData = runtime.lookupFunction(addresses.selectGuitarProviderInitData);
        runtime.replaceFunction(addresses.selectGuitarProviderInitData, &guitarsInitData);
        EntryHook<ShowCharTag>::install(runtime, addresses.charsysPanelShowChar, onShowChar);
        EntryHook<LoadGuitarTag>::install(runtime, addresses.guitarElLoadGuitar, onLoadGuitar);
    }
}
