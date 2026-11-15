#include "content/sources.h"

#include "disc/ark.h"
#include "formats/dtb.h"
#include "guest.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>

namespace gh2::sources
{
    namespace
    {
        using Names = std::set<std::string>;
        // A name on the disc to its id.
        using Ids = std::map<std::string, std::string>;

        // Config scripts naming a song only as one: its entry, its tier, its
        // price, its co-op scores.
        constexpr const char *kConfigs[] = {"config/gen/songs.dtb", "config/gen/campaign.dtb",
                                            "config/gen/store.dtb", "config/gen/coop_max_scores.dtb"};
        // Screens naming one: the store's stand-in logos (career), the
        // encore's effects (game), the HUD budget test songs, the tutorials.
        constexpr const char *kScreens[] = {"ui/gen/career.dtb", "ui/gen/game.dtb", "ui/gen/hud_budget.dtb",
                                            "ui/gen/tutorials.dtb"};
        // Song names that are also ordinary words in those screens:
        // {meta_music stop} is no song.
        const Names kWords = {"stop"};

        // Names a script makes from a venue or character to find an object
        // in a scene, made from its own name: a venue's button on the venue
        // map ({sprintf "sv_%s.btn" ...}, career.dta's venue_button), a
        // character's portrait ({sprint "char_" $c ".mesh"}, career.dta and
        // multiplayer.dta's pickers).
        constexpr const char *kOwnFormats[] = {"sv_%s.btn"};
        constexpr const char *kOwnPrefixes[] = {"char_"};

        // Each symbol that is one of `names` as `source` names it.
        void qualify(dtb::Node &node, const std::string &source, const Names &names, const Names &skip)
        {
            if (node.type == dtb::kSymbol && names.count(node.text) != 0u && skip.count(node.text) == 0u)
                node.text = id(source, node.text);
            for (dtb::Node &n : node.nodes)
                qualify(n, source, names, skip);
        }

        bool isSymbol(const dtb::Node &node, const char *text)
        {
            return node.type == dtb::kSymbol && node.text == text;
        }

        // Every venue's name in it.
        void rename(dtb::Node &node, const Ids &ids)
        {
            if (node.type == dtb::kSymbol)
                if (const auto it = ids.find(node.text); it != ids.end())
                    node.text = it->second;
            for (dtb::Node &n : node.nodes)
                rename(n, ids);
        }

        // {game get_venue}, {gamecfg get_venue}
        bool isGetVenue(const dtb::Node &node)
        {
            return node.type == dtb::kCommand && node.nodes.size() == 2u && isSymbol(node.nodes[1], "get_venue");
        }

        // A venue wherever a script names one as a venue, never a word that
        // is also a venue's name elsewhere ({arena ...}, big): in a (venues
        // ...) or (venue ...) array (the list, a song's quickplay venue,
        // DX's achievements by venue), what {game set_venue} or {game
        // set_career_venue} is given (marfGH's {random_elem (...)} too), and
        // what {== <x> get_venue} is compared with either way round. A
        // venue made into an object's name gets its own name.
        void qualifyVenues(dtb::Node &node, const Ids &ids)
        {
            auto &n = node.nodes;
            if (node.type == dtb::kArray && !n.empty() && (isSymbol(n[0], "venues") || isSymbol(n[0], "venue")))
                for (size_t i = 1u; i < n.size(); ++i)
                    rename(n[i], ids);
            if (node.type == dtb::kCommand && n.size() == 3u && isSymbol(n[0], "game") &&
                (isSymbol(n[1], "set_venue") || isSymbol(n[1], "set_career_venue")))
                rename(n[2], ids);
            if (node.type == dtb::kCommand && n.size() == 3u && (isSymbol(n[0], "==") || isSymbol(n[0], "!=")))
            {
                if (isGetVenue(n[1]))
                    rename(n[2], ids);
                if (isGetVenue(n[2]))
                    rename(n[1], ids);
            }
            for (dtb::Node &child : n)
                qualifyVenues(child, ids);
        }

        // Any name a script makes into an object's ({sprintf "sv_%s.btn"
        // $v}, {sprint "char_" $c ".mesh"}) made from what it is given's own
        // name.
        void ownNames(dtb::Node &node)
        {
            auto &n = node.nodes;
            const auto own = [](dtb::Node &arg) { arg = dtb::command({dtb::symbol("sources"), dtb::symbol("own"), arg}); };
            if (node.type == dtb::kCommand && n.size() >= 3u && isSymbol(n[0], "sprintf") && n[1].type == dtb::kString)
                for (const char *format : kOwnFormats)
                    if (n[1].text == format)
                        for (size_t i = 2u; i < n.size(); ++i)
                            own(n[i]);
            if (node.type == dtb::kCommand && n.size() >= 3u && isSymbol(n[0], "sprint") && n[1].type == dtb::kString)
                for (const char *prefix : kOwnPrefixes)
                    if (n[1].text == prefix)
                        own(n[2]);
            for (dtb::Node &child : n)
                ownNames(child);
        }

        // A list's entry: a name, or the name of a (<name> ...); never a
        // command ((character {...}), a switch's case).
        void renameEntry(dtb::Node &entry, const Ids &ids)
        {
            if (entry.type == dtb::kSymbol)
                rename(entry, ids);
            else if (entry.type == dtb::kArray && !entry.nodes.empty())
                rename(entry.nodes[0], ids);
        }

        // The (<name> ...) entries of a list: their names.
        void renameKeys(dtb::Node &list, const Ids &ids)
        {
            for (size_t i = 1u; i < list.nodes.size(); ++i)
                if (list.nodes[i].type == dtb::kArray && !list.nodes[i].nodes.empty())
                    rename(list.nodes[i].nodes[0], ids);
        }

        // The UI's lists of characters and of outfits (ui.dta).
        constexpr const char *kCharacterLists[] = {"CHARACTERS", "STORE_CHARACTERS"};
        constexpr const char *kOutfitLists[] = {"LOAD_CHARACTERS", "STORE_OUTFITS"};

        // A character or outfit wherever a script names one as one, never a
        // word that is also one's name elsewhere (the classic look, the
        // store's (outfit ...) of a guitar): config's (characters (<c> (<o>)
        // ...) ...), the store's (character (<c> ...)) and (outfit (<o> ...)),
        // the UI's lists, a song's (character_outfit <o>) and (band <o> ...),
        // the default band, the career picker's (char_focus <c>) and
        // (char_outfit <o>), and what {<x> set_character <o>} is given.
        void qualifyCast(dtb::Node &node, const Ids &characters, const Ids &outfits)
        {
            auto &n = node.nodes;
            const auto head = [&](const char *name) { return node.type == dtb::kArray && !n.empty() && isSymbol(n[0], name); };
            if (head("characters"))
                for (size_t i = 1u; i < n.size(); ++i)
                    if (n[i].type == dtb::kArray && !n[i].nodes.empty())
                    {
                        rename(n[i].nodes[0], characters);
                        renameKeys(n[i], outfits);
                    }
            if (head("character"))
                for (size_t i = 1u; i < n.size(); ++i)
                    renameEntry(n[i], characters);
            if (head("outfit"))
                renameKeys(node, outfits);
            if (n.size() == 2u && (head("character_outfit") || head("char_outfit")))
                rename(n[1], outfits);
            if (n.size() == 2u && head("char_focus"))
                rename(n[1], characters);
            if (head("band") || head("default_band"))
                for (size_t i = 1u; i < n.size(); ++i)
                    rename(n[i], outfits);
            if (node.type == dtb::kCommand && n.size() >= 3u && isSymbol(n[1], "set_character"))
                rename(n[2], outfits);
            for (size_t i = 0u; i + 1u < n.size(); ++i)
                if (n[i].type == dtb::kDefine)
                {
                    for (const char *list : kCharacterLists)
                        if (n[i].text == list)
                            rename(n[i + 1u], characters);
                    for (const char *list : kOutfitLists)
                        if (n[i].text == list)
                            rename(n[i + 1u], outfits);
                }
            for (dtb::Node &child : n)
                qualifyCast(child, characters, outfits);
        }

        // Paths under songs/<name>/ moved to songs/<ours>/, the files named
        // by the song with it.
        void retarget(dtb::Node &node, const std::string &source, const std::string &name)
        {
            const std::string file = "songs/" + name + "/" + name, dir = "songs/" + name + "/";
            const std::string ours = id(source, name);
            if (node.type == dtb::kSymbol || node.type == dtb::kString)
            {
                if (node.text.rfind(file, 0) == 0)
                    node.text = "songs/" + ours + "/" + ours + node.text.substr(file.size());
                else if (node.text.rfind(dir, 0) == 0)
                    node.text = "songs/" + ours + "/" + node.text.substr(dir.size());
            }
            for (dtb::Node &n : node.nodes)
                retarget(n, source, name);
        }

        // That disc's `path`, read, changed by `change` and written back.
        void fix(size_t disc, const std::string &path, std::function<void(dtb::Node &)> change)
        {
            ark::addFix(disc, path, [path, change = std::move(change)](std::vector<uint8_t> &bytes)
                        {
                            auto root = dtb::raw(bytes);
                            if (!root)
                            {
                                std::cerr << "[sources] cannot read " << path << std::endl;
                                return;
                            }
                            change(*root);
                            bytes = dtb::write(*root);
                        });
        }

        // A disc's names by kind, each to its id.
        struct Named
        {
            std::map<Kind, Ids> ids;

            const Ids &of(Kind kind) const
            {
                static const Ids s_none;
                const auto it = ids.find(kind);
                return it == ids.end() ? s_none : it->second;
            }
        };

        // The locale keys `key` is as ids: a name of anything the disc has,
        // or one made from one. A name of two kinds (grim) is both keys.
        std::vector<std::string> localeKeys(const std::string &key, const Named &named)
        {
            std::vector<std::string> keys;
            for (const auto &[kind, ids] : named.ids)
                if (const auto it = ids.find(key); it != ids.end())
                    keys.push_back(it->second);
            for (const KeyForm &form : kKeyForms)
            {
                const std::string prefix = form.prefix, suffix = form.suffix;
                if (key.size() <= prefix.size() + suffix.size() || !key.starts_with(prefix) || !key.ends_with(suffix))
                    continue;
                const Ids &ids = named.of(form.kind);
                if (const auto it = ids.find(key.substr(prefix.size(), key.size() - prefix.size() - suffix.size()));
                    it != ids.end())
                    keys.push_back(prefix + it->second + suffix);
            }
            // GH2's grim is one id either way.
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            return keys;
        }

        // The venues on that disc: its config's list (config/gh2.dta's
        // venues), and any world/<v>/ with its scene in world/<v>/gen/.
        Names venuesOn(size_t disc)
        {
            Names found;
            if (const auto file = ark::readFile(disc, "config/gen/gh2.dtb"))
                if (const auto root = dtb::raw(*file))
                    if (const dtb::Node *list = dtb::find(*root, "venues"))
                        for (size_t i = 1u; i < list->nodes.size(); ++i)
                            if (list->nodes[i].type == dtb::kSymbol)
                                found.insert(list->nodes[i].text);
            for (const std::string &path : ark::paths(disc, "world/"))
            {
                const size_t slash = path.find('/', 6u);
                if (slash == std::string::npos)
                    continue;
                const std::string venue = path.substr(6u, slash - 6u);
                if (path == "world/" + venue + "/gen/" + venue + ".milo_ps2")
                    found.insert(venue);
            }
            return found;
        }

        // The characters and outfits on that disc: its config's (characters
        // (<c> (<o>) ...) ...), and any char/<o>/ with char/<o>/og/<o>.milo,
        // the band among them.
        std::pair<Names, Names> castOn(size_t disc)
        {
            Names characters, outfits;
            if (const auto file = ark::readFile(disc, "config/gen/gh2.dtb"))
                if (const auto root = dtb::raw(*file))
                    if (const dtb::Node *list = dtb::find(*root, "characters"))
                        for (size_t i = 1u; i < list->nodes.size(); ++i)
                        {
                            const dtb::Node &c = list->nodes[i];
                            if (c.type != dtb::kArray || c.nodes.empty() || c.nodes[0].type != dtb::kSymbol)
                                continue;
                            characters.insert(c.nodes[0].text);
                            for (size_t j = 1u; j < c.nodes.size(); ++j)
                                if (c.nodes[j].type == dtb::kArray && !c.nodes[j].nodes.empty())
                                    outfits.insert(c.nodes[j].nodes[0].text);
                        }
            for (const std::string &path : ark::paths(disc, "char/"))
            {
                const size_t slash = path.find('/', 5u);
                if (slash == std::string::npos)
                    continue;
                const std::string outfit = path.substr(5u, slash - 5u);
                if (path == "char/" + outfit + "/og/gen/" + outfit + ".milo_ps2")
                    outfits.insert(outfit);
            }
            return {characters, outfits};
        }

        // The guitars, finishes and models on that disc: its guitars.dta's
        // (<g> (type ...) (skins (<f> (outfit <m>) (mat ...)) ...)), and any
        // char/og/guitars/<m>.milo (the air guitar's). None on a disc with no
        // guitar scenes there: GH1's are GH1's own form (charsys/guitars/),
        // which its career converts (gh1/guitars.h).
        struct Guitars
        {
            Names guitars, finishes, models;
        };
        Guitars guitarsOn(size_t disc)
        {
            Guitars found;
            const std::string folder = "char/og/guitars/gen/", suffix = ".milo_ps2";
            const std::vector<std::string> scenes = ark::paths(disc, folder);
            if (scenes.empty())
                return found;
            if (const auto file = ark::readFile(disc, "config/gen/guitars.dtb"))
                if (const auto root = dtb::raw(*file))
                    for (const dtb::Node &guitar : root->nodes)
                    {
                        if (guitar.type != dtb::kArray || guitar.nodes.empty() || guitar.nodes[0].type != dtb::kSymbol)
                            continue;
                        found.guitars.insert(guitar.nodes[0].text);
                        if (const dtb::Node *skins = dtb::find(guitar, "skins"))
                            for (size_t i = 1u; i < skins->nodes.size(); ++i)
                                if (skins->nodes[i].type == dtb::kArray && !skins->nodes[i].nodes.empty())
                                {
                                    found.finishes.insert(skins->nodes[i].nodes[0].text);
                                    if (const dtb::Node *model = dtb::find(skins->nodes[i], "outfit"); model && model->nodes.size() > 1u)
                                        found.models.insert(model->nodes[1].text);
                                }
                    }
            for (const std::string &path : scenes)
                if (path.ends_with(suffix) && path.find('/', folder.size()) == std::string::npos)
                    found.models.insert(path.substr(folder.size(), path.size() - folder.size() - suffix.size()));
            return found;
        }

        // A guitar, finish or model wherever a script names one as one, never
        // a word that is also one's name elsewhere (a guitar's (type guitar),
        // the controller's guitar): guitars.dta's entries, the store's
        // (guitar (<g> ...)) and (skin (<f> ...)), a song's (guitar <g>),
        // what {<x> set_guitar <g> <f>} is given, and the air guitar cheat's
        // model ($cheat_guitar).
        void qualifyGuitars(dtb::Node &node, const Named &named, bool config)
        {
            const Ids &guitars = named.of(Kind::Guitar), &finishes = named.of(Kind::Finish), &models = named.of(Kind::Model);
            auto &n = node.nodes;
            const auto head = [&](const char *name) { return node.type == dtb::kArray && !n.empty() && isSymbol(n[0], name); };
            if (config)
                for (dtb::Node &guitar : n)
                    if (guitar.type == dtb::kArray && !guitar.nodes.empty() && guitars.count(guitar.nodes[0].text) != 0u)
                    {
                        rename(guitar.nodes[0], guitars);
                        for (dtb::Node &skins : guitar.nodes)
                            if (skins.type == dtb::kArray && !skins.nodes.empty() && isSymbol(skins.nodes[0], "skins"))
                                for (size_t i = 1u; i < skins.nodes.size(); ++i)
                                    if (dtb::Node &skin = skins.nodes[i]; skin.type == dtb::kArray && !skin.nodes.empty())
                                    {
                                        rename(skin.nodes[0], finishes);
                                        for (dtb::Node &model : skin.nodes)
                                            if (model.type == dtb::kArray && model.nodes.size() > 1u && isSymbol(model.nodes[0], "outfit"))
                                                rename(model.nodes[1], models);
                                    }
                    }
            if (head("guitar"))
                for (size_t i = 1u; i < n.size(); ++i)
                    renameEntry(n[i], guitars);
            if (head("skin"))
                renameKeys(node, finishes);
            if (node.type == dtb::kCommand && n.size() >= 3u && isSymbol(n[1], "set_guitar"))
            {
                rename(n[2], guitars);
                if (n.size() >= 4u)
                    rename(n[3], finishes);
            }
            if (node.type == dtb::kCommand && n.size() == 3u && n[1].type == dtb::kVar && n[1].text == "cheat_guitar")
                rename(n[2], models);
            if (node.type == dtb::kCommand && n.size() == 3u && n[2].type == dtb::kVar && n[2].text == "cheat_guitar")
                rename(n[1], models);
            for (dtb::Node &child : n)
                qualifyGuitars(child, named, false);
        }

        // GameConfig::GetVenue (0x1264d0) gives the venue's own name while
        // the game makes a name from it to find something in a file:
        // OnFilterClips' <venue>_exclude and <venue>_multi_exclude in the
        // character's clips (0x1293e8), OnChangeDrums' kit with the venue in
        // its name (strstr, 0x129248).
        const Addresses *s_addresses = nullptr;
        int s_makingNames = 0;
        PS2Runtime::RecompiledFunction s_getVenue = nullptr, s_onFilterClips = nullptr, s_onChangeDrums = nullptr;

        // The Symbol of that venue's own name.
        uint32_t ownSymbol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t venue)
        {
            if (venue == 0u)
                return venue;
            const std::string ours = reinterpret_cast<const char *>(getMemPtr(rdram, venue));
            const std::string name = own(ours);
            return name == ours ? venue : script::symbol(rdram, ctx, runtime, name);
        }

        void getVenue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            s_getVenue(rdram, ctx, runtime);
            if (s_makingNames > 0)
                SET_GPR_U32(ctx, 2, ownSymbol(rdram, ctx, runtime, GPR_U32(ctx, 2)));
            ctx->pc = returnTo;
        }

        // Each returns a DataNode by the pointer in a0: (DataNode *, this,
        // DataArray *).
        template <PS2Runtime::RecompiledFunction *Original, uint32_t Addresses::*Function>
        void makingNames(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            ++s_makingNames;
            const auto result = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, s_addresses->*Function,
                                           {GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6)}, *Original));
            --s_makingNames;
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = returnTo;
        }

        // GameConfig::VenueData(Symbol venue) (0x126d80) is the config's
        // WorldDir type of that name (objects WorldDir types <venue>), which
        // the venue's own scene names it by.
        PS2Runtime::RecompiledFunction s_venueData = nullptr;

        void venueData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t venue = ownSymbol(rdram, ctx, runtime, GPR_U32(ctx, 5));
            const auto data = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->gameConfigVenueData, {GPR_U32(ctx, 4), venue}, s_venueData));
            SET_GPR_U32(ctx, 2, data);
            ctx->pc = returnTo;
        }

        // VenueProvider keeps each venue's WorldDir type (InitData, 0x118a90,
        // through VenueData), and names a venue by the type's name, its own
        // name: GetVenue(int) (0x118b98) gives the id of that own name in
        // the config's venues list, which it was made from, and
        // GetIndex(Symbol) (0x118bc8) finds an id by its own name.
        PS2Runtime::RecompiledFunction s_providerGetVenue = nullptr, s_providerGetIndex = nullptr;

        void providerGetVenue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            uint32_t venue = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->venueProviderGetVenue, {GPR_U32(ctx, 4), GPR_U32(ctx, 5)}, s_providerGetVenue));
            const std::string name = venue ? reinterpret_cast<const char *>(getMemPtr(rdram, venue)) : "";
            const uint32_t list = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->systemConfig1, {script::symbol(rdram, ctx, runtime, "venues")}));
            for (int i = 1, n = list ? load<int16_t>(rdram, list + 8u) : 0; i < n; ++i)
            {
                const uint32_t node = load<uint32_t>(rdram, list) + 8u * static_cast<uint32_t>(i);
                const uint32_t symbol = load<uint32_t>(rdram, node);
                if (load<uint32_t>(rdram, node + 4u) == script::kSymbol &&
                    own(reinterpret_cast<const char *>(getMemPtr(rdram, symbol))) == name)
                    venue = symbol;
            }
            SET_GPR_U32(ctx, 2, venue);
            ctx->pc = returnTo;
        }

        void providerGetIndex(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t venue = ownSymbol(rdram, ctx, runtime, GPR_U32(ctx, 5));
            const auto index = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->venueProviderGetIndex, {GPR_U32(ctx, 4), venue}, s_providerGetIndex));
            SET_GPR_U32(ctx, 2, index);
            ctx->pc = returnTo;
        }

        // Symbol::Symbol(const char *) (0x2d3a48) made from the executable's
        // "scythe" (the Grim Ripper's guitar) is made from its id: GH2's,
        // whichever disc's guitar of that name it is.
        struct SymbolTag;
        uint32_t s_scythe = 0u;
        void onSymbol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 5) != s_addresses->scytheText)
                return;
            if (s_scythe == 0u)
            {
                const EntryArgs args(ctx);
                s_scythe = script::symbol(rdram, ctx, runtime, id("gh2", "scythe"));
                args.restore(ctx);
            }
            SET_GPR_U32(ctx, 5, s_scythe);
        }

        // ProfileState::InitGuitars (0x13a888) makes each guitar's item
        // (CampaignItem::CampaignItem(Type, Symbol, bool unlocked, ...),
        // 0x140950) unlocked unless strcmp says it is named "battleaxe", the
        // guitar a band is handed: so the one of that own name too.
        struct CampaignItemTag;
        void onCampaignItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t from = GPR_U32(ctx, 31), name = GPR_U32(ctx, 6);
            if (from < s_addresses->profileStateInitGuitars || from >= s_addresses->profileStateInitGuitarsEnd || name == 0u)
                return;
            const Row *row = find(reinterpret_cast<const char *>(getMemPtr(rdram, name)), Kind::Guitar);
            if (row && row->own == "battleaxe")
                SET_GPR_U32(ctx, 8, 0u);
        }

        script::Node sourcesCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "own")
                return call.word(own(call.text(2)));
            if (op == "owned")
            {
                // A DataArray: its nodes at +0, 8 bytes each, as many as +8.
                const script::Node list = call.arg(2);
                const std::string name = call.text(3);
                if (list.type == script::kArray)
                    for (int i = 0, n = load<int16_t>(call.rdram, list.value + 8u); i < n; ++i)
                    {
                        const uint32_t node = load<uint32_t>(call.rdram, list.value) + 8u * static_cast<uint32_t>(i);
                        if (load<uint32_t>(call.rdram, node + 4u) == script::kSymbol &&
                            own(reinterpret_cast<const char *>(getMemPtr(call.rdram, load<uint32_t>(call.rdram, node)))) == name)
                            return {1u, script::kInt};
                    }
                return {0u, script::kInt};
            }
            return {};
        }
    }

    void entry(dtb::Node &entry, const std::string &source)
    {
        if (entry.type != dtb::kArray || entry.nodes.empty())
            return;
        const std::string name = entry.nodes[0].text;
        entry.nodes[0].text = id(source, name);
        retarget(entry, source, name);
    }

    void addDisc(const std::string &source, size_t disc, const std::vector<std::string> &unlisted)
    {
        const auto file = ark::readFile(disc, kConfigs[0]);
        const auto songs = file ? dtb::raw(*file) : std::nullopt;
        if (!songs)
        {
            std::cerr << "[" << source << "] cannot read its songs" << std::endl;
            return;
        }
        auto names = std::make_shared<Names>();
        for (const dtb::Node &entry : songs->nodes)
            if (entry.type == dtb::kArray && !entry.nodes.empty() && entry.nodes[0].type == dtb::kSymbol)
                names->insert(entry.nodes[0].text);
        names->insert(unlisted.begin(), unlisted.end());
        for (const std::string &name : *names)
            add(Kind::Song, source, name);
        auto named = std::make_shared<Named>();
        for (const std::string &name : *names)
            named->ids[Kind::Song][name] = id(source, name);
        for (const std::string &venue : venuesOn(disc))
            named->ids[Kind::Venue][venue] = add(Kind::Venue, source, venue).id;
        const auto [castCharacters, castOutfits] = castOn(disc);
        for (const std::string &character : castCharacters)
            named->ids[Kind::Character][character] = addShared(Kind::Character, source, character).id;
        for (const std::string &outfit : castOutfits)
            named->ids[Kind::Outfit][outfit] = add(Kind::Outfit, source, outfit).id;
        const Guitars guitars = guitarsOn(disc);
        for (const std::string &guitar : guitars.guitars)
            named->ids[Kind::Guitar][guitar] = addShared(Kind::Guitar, source, guitar).id;
        for (const std::string &finish : guitars.finishes)
            named->ids[Kind::Finish][finish] = addShared(Kind::Finish, source, finish).id;
        for (const std::string &model : guitars.models)
            named->ids[Kind::Model][model] = add(Kind::Model, source, model).id;
        const auto venues = std::make_shared<Ids>(named->of(Kind::Venue));

        fix(disc, kConfigs[0], [source, names](dtb::Node &root)
            {
                for (dtb::Node &e : root.nodes)
                    if (e.type == dtb::kArray && !e.nodes.empty() && names->count(e.nodes[0].text) != 0u)
                        entry(e, source);
            });
        for (size_t i = 1u; i < std::size(kConfigs); ++i)
            fix(disc, kConfigs[i], [source, names](dtb::Node &root) { qualify(root, source, *names, {}); });
        // A tier of a campaign is its venue's: campaign.dtb's, and DX's
        // others (campaign_ce.dtb).
        for (const std::string &path : ark::paths(disc, "config/gen/campaign"))
            fix(disc, path, [venues](dtb::Node &root)
                {
                    for (dtb::Node &node : root.nodes)
                        for (dtb::Node &tier : node.nodes)
                            if (tier.type == dtb::kArray && !tier.nodes.empty())
                                rename(tier.nodes[0], *venues);
                });

        std::vector<std::string> screens(std::begin(kScreens), std::end(kScreens));
        for (const std::string &path : ark::paths(disc, "ui/gen/tutorial_"))
            screens.push_back(path);
        for (const std::string &path : screens)
            fix(disc, path, [source, names](dtb::Node &root) { qualify(root, source, *names, kWords); });
        for (const char *prefix : {"config/gen/", "ui/gen/"})
            for (const std::string &path : ark::paths(disc, prefix))
                if (path.ends_with(".dtb"))
                    fix(disc, path, [named, path](dtb::Node &root)
                        {
                            qualifyVenues(root, named->of(Kind::Venue));
                            qualifyCast(root, named->of(Kind::Character), named->of(Kind::Outfit));
                            qualifyGuitars(root, *named, path == "config/gen/guitars.dtb");
                            ownNames(root);
                        });

        // A guitar's OutfitLoader loads the scenes its category lists
        // (char_objects.dta's (guitars (<m>) ...)) and no other.
        fix(disc, "char/gen/char_objects.dtb", [named](dtb::Node &root)
            {
                const std::function<void(dtb::Node &)> walk = [&](dtb::Node &node)
                {
                    if (node.type == dtb::kArray && !node.nodes.empty() && isSymbol(node.nodes[0], "guitars"))
                        renameKeys(node, named->of(Kind::Model));
                    for (dtb::Node &n : node.nodes)
                        walk(n);
                };
                walk(root);
            });

        // A song's title is the locale token of its name, a venue's,
        // character's or outfit's of its: one entry for each it names.
        fix(disc, "ui/eng/gen/locale.dtb", [source, named](dtb::Node &root)
            {
                std::vector<dtb::Node> entries;
                for (dtb::Node &entry : root.nodes)
                {
                    if (entry.type != dtb::kArray || entry.nodes.empty() || entry.nodes[0].type != dtb::kSymbol)
                    {
                        entries.push_back(std::move(entry));
                        continue;
                    }
                    const std::vector<std::string> keys = localeKeys(entry.nodes[0].text, *named);
                    for (size_t i = 1u; i < keys.size(); ++i)
                    {
                        entries.push_back(entry);
                        entries.back().nodes[0].text = keys[i];
                    }
                    if (!keys.empty())
                        entry.nodes[0].text = keys[0];
                    entries.push_back(std::move(entry));
                }
                root.nodes = std::move(entries);
            });

        // The specific before the folder: the first rename that matches wins.
        for (const std::string &name : *names)
        {
            const std::string ours = id(source, name);
            ark::rename("songs/" + ours + "/" + ours, disc, "songs/" + name + "/" + name);
            ark::rename("songs/" + ours + "/", disc, "songs/" + name + "/");
        }
        // world/<id>/<id>_bank.milo is world/<v>/<v>_bank.milo, and so on.
        for (const auto &[name, ours] : *venues)
        {
            ark::rename("world/" + ours + "/" + ours, disc, "world/" + name + "/" + name);
            ark::rename("world/" + ours + "/gen/" + ours, disc, "world/" + name + "/gen/" + name);
            ark::rename("world/" + ours + "/", disc, "world/" + name + "/");
        }
        // char/<id>/og/<id>.milo is char/<o>/og/<o>.milo, and so on; the
        // highway (track/surfaces/%s_keep.bmp) and the results' photos
        // (ui/image/og/photo_%s%i_keep.bmp) go by outfit too.
        for (const auto &[name, ours] : named->of(Kind::Outfit))
        {
            ark::rename("char/" + ours + "/og/gen/" + ours, disc, "char/" + name + "/og/gen/" + name);
            ark::rename("char/" + ours + "/", disc, "char/" + name + "/");
            ark::rename("track/surfaces/gen/" + ours + "_keep", disc, "track/surfaces/gen/" + name + "_keep");
            for (int i = 0; i < 4; ++i)
            {
                const std::string index = std::to_string(i) + "_keep";
                ark::rename("ui/image/og/gen/photo_" + ours + index, disc, "ui/image/og/gen/photo_" + name + index);
            }
        }
        // char/og/guitars/<id>.milo (the executable's char/og/guitars/%s.milo)
        // is char/og/guitars/<m>.milo.
        for (const auto &[name, ours] : named->of(Kind::Model))
            ark::rename("char/og/guitars/gen/" + ours + ".", disc, "char/og/guitars/gen/" + name + ".");
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("sources", sourcesCommand);
        EntryHook<SymbolTag>::install(runtime, addresses.symbolCtor, onSymbol);
        EntryHook<CampaignItemTag>::install(runtime, addresses.campaignItemCtor, onCampaignItem);
        s_getVenue = runtime.lookupFunction(addresses.gameConfigGetVenue);
        runtime.replaceFunction(addresses.gameConfigGetVenue, &getVenue);
        s_onFilterClips = runtime.lookupFunction(addresses.gameConfigOnFilterClips);
        runtime.replaceFunction(addresses.gameConfigOnFilterClips,
                                &makingNames<&s_onFilterClips, &Addresses::gameConfigOnFilterClips>);
        s_venueData = runtime.lookupFunction(addresses.gameConfigVenueData);
        runtime.replaceFunction(addresses.gameConfigVenueData, &venueData);
        s_providerGetVenue = runtime.lookupFunction(addresses.venueProviderGetVenue);
        runtime.replaceFunction(addresses.venueProviderGetVenue, &providerGetVenue);
        s_providerGetIndex = runtime.lookupFunction(addresses.venueProviderGetIndex);
        runtime.replaceFunction(addresses.venueProviderGetIndex, &providerGetIndex);
        s_onChangeDrums = runtime.lookupFunction(addresses.gameConfigOnChangeDrums);
        runtime.replaceFunction(addresses.gameConfigOnChangeDrums,
                                &makingNames<&s_onChangeDrums, &Addresses::gameConfigOnChangeDrums>);
    }
}
