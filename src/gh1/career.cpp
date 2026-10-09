// GH1's career as a campaign (content/campaigns.h). GH1's archive is its own
// engine's, so nothing of it can stand in front: the campaign is the game
// disc with GH1's campaign, store, tips, guitarists and text over it, each
// made here in GH2's form from GH1's own.
//
//   campaign    GH1's tiers and cash, each tier in the GH2 venue standing in
//               for its own (gh1/songs.h), with no encores
//               (content/encores.h)
//   venues      GH1's rooms in those GH2 venues (gh1/venues.cpp)
//   store       GH1's songs, characters and guitars at GH1's prices,
//               the guitars GH2's models of the same Gibsons. GH1's finishes
//               are not GH2's, so each body's finishes are GH2's at the
//               prices of GH1's for that body, in order.
//   guitars     GH2's, less the bodies GH1 has none of
//   characters  GH1's eight, each its GH1 guitarist (gh1/install.cpp), in
//               the root config and the UI's lists of them
//   text        GH2's locale with GH1's names and blurbs for the venues,
//               characters, videos, songs and tips
//   menus       GH1's screens and their text (gh1/menus.h), under GH2's
//               scripts fitted to them
//   music       GH1's menu loops

#include "gh1/career.h"

#include "content/campaigns.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/menu_scripts.h"
#include "gh1/menus.h"
#include "gh1/songs.h"
#include "gh1/venues.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>

namespace gh2::gh1
{
    namespace
    {
        using dtb::array;
        using dtb::symbol;

        std::string keyOf(const dtb::Node &node)
        {
            return node.type == dtb::kArray && !node.nodes.empty() ? node.nodes[0].text : node.text;
        }

        dtb::Node *child(dtb::Node &parent, const std::string &key)
        {
            for (dtb::Node &n : parent.nodes)
                if (n.type == dtb::kArray && keyOf(n) == key)
                    return &n;
            return nullptr;
        }

        // GH1's Gibsons by GH2's names for the same bodies.
        std::string body(std::string gh1)
        {
            static const std::map<std::string, std::string> kBodies = {
                {"xplorer", "explorer"}, {"lespaul_special", "lespaul_doublecut"}};
            if (gh1.rfind("gibson_", 0) == 0)
                gh1 = gh1.substr(7);
            const auto it = kBodies.find(gh1);
            return it != kBodies.end() ? it->second : gh1;
        }

        // (key (price n))
        dtb::Node priced(const std::string &key, int32_t price)
        {
            return array({symbol(key), array({symbol("price"), {dtb::kInt, price, 0.0f, {}, {}}})});
        }

        int32_t priceOf(const dtb::Node &item)
        {
            const dtb::Node *price = dtb::find(item, "price");
            return price && price->nodes.size() > 1u ? price->nodes[1].integer : 0;
        }

        // Every symbol named `from` under `node`, as `to`.
        void replaceSymbol(dtb::Node &node, const std::string &from, const std::string &to)
        {
            if (node.type == dtb::kSymbol && node.text == from)
                node.text = to;
            for (dtb::Node &n : node.nodes)
                replaceSymbol(n, from, to);
        }

        // A #define's body, the array after it.
        void define(dtb::Node &root, const std::string &name, dtb::Node body)
        {
            for (size_t i = 0; i + 1u < root.nodes.size(); ++i)
                if (root.nodes[i].type == dtb::kDefine && root.nodes[i].text == name)
                    root.nodes[i + 1u] = std::move(body);
        }

        std::optional<dtb::Node> raw(const std::string &path)
        {
            const auto file = ark::readFile(0u, path);
            return file ? dtb::raw(*file) : std::nullopt;
        }

        // `key` with a part of it that is a GH1 name (whole, or set off by
        // underscores) as GH2's for the same thing, or none if it has no
        // such part.
        std::optional<std::string> renamed(const std::string &key, const std::map<std::string, std::string> &names)
        {
            // Longest first: hair_metal before metal.
            std::vector<std::pair<std::string, std::string>> byLength(names.begin(), names.end());
            std::sort(byLength.begin(), byLength.end(),
                      [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
            for (const auto &[from, to] : byLength)
                for (size_t at = key.find(from); at != std::string::npos; at = key.find(from, at + 1u))
                {
                    const size_t end = at + from.size();
                    if ((at == 0u || key[at - 1u] == '_') && (end == key.size() || key[end] == '_'))
                        return key.substr(0, at) + to + key.substr(end);
                }
            return std::nullopt;
        }
    }

    void addCareer(size_t disc, const std::vector<Guitarist> &guitarists)
    {
        const dtb::Files theirs = [disc](const std::string &path) { return ark::readFile(disc, path); };
        const dtb::Files game = [](const std::string &path) { return ark::readFile(0u, path); };
        dtb::Macros none;
        auto campaign = dtb::read("config/campaign.dta", none, theirs);
        const auto theirStore = dtb::read("config/store.dta", none, theirs);
        const auto theirGuitars = dtb::read("config/guitars.dta", none, theirs);
        const auto theirTips = dtb::read("config/tips.dta", none, theirs);
        const auto strings = dtb::read("ghui/eng/locale.dta", none, theirs);
        const auto gameCampaign = dtb::read("config/campaign.dta", none, game);
        const auto gameStore = dtb::read("config/store.dta", none, game);
        auto guitars = dtb::read("config/guitars.dta", none, game);
        auto tips = dtb::read("config/tips.dta", none, game);
        auto root = raw("config/gen/gh2.dtb");
        auto ui = raw("ui/gen/ui.dtb");
        auto locale = raw("ui/eng/gen/locale.dtb");
        if (!campaign || !theirStore || !theirGuitars || !theirTips || !strings || !gameCampaign || !gameStore ||
            !guitars || !tips || !root || !ui || !locale || guitarists.empty())
        {
            std::cerr << "[gh1] cannot read GH1's career" << std::endl;
            return;
        }

        // Campaign: the tiers in their stand-in venues, a count for the last
        // venue as GH2 keeps one, and what GH2's has that GH1's lacks.
        std::map<std::string, std::string> names; // GH1's to GH2's, for the text
        dtb::Node venues = array({symbol("venues")});
        if (dtb::Node *order = child(*campaign, "order"))
            for (size_t i = 1u; i < order->nodes.size(); ++i)
                if (!order->nodes[i].nodes.empty())
                {
                    std::string &tier = order->nodes[i].nodes[0].text;
                    names[tier] = venue(tier);
                    tier = venue(tier);
                    venues.nodes.push_back(symbol(tier));
                }
        if (dtb::Node *required = child(*campaign, "required_songs"))
            for (size_t d = 1u; d < required->nodes.size(); ++d)
                if (required->nodes[d].nodes.size() == 2u)
                    required->nodes[d].nodes.push_back(required->nodes[d].nodes[1]);
        for (const dtb::Node &n : gameCampaign->nodes)
            if (n.type == dtb::kArray && !dtb::find(*campaign, keyOf(n)))
                campaign->nodes.push_back(n);

        // Guitars: the bodies GH1 has, and everything that is no body of
        // either's (basses, the scythe, the cheat's).
        std::set<std::string> bodies;
        std::map<std::string, std::vector<int32_t>> finishPrices; // by body, GH1's store's
        std::map<std::string, std::string> bodyOfTheirSkin;
        for (const dtb::Node &guitar : theirGuitars->nodes)
        {
            bodies.insert(body(keyOf(guitar)));
            if (const dtb::Node *skins = dtb::find(guitar, "skins"))
                for (size_t i = 1u; i < skins->nodes.size(); ++i)
                    bodyOfTheirSkin[keyOf(skins->nodes[i])] = body(keyOf(guitar));
        }
        bodies.insert("battleaxe_reward");
        if (const dtb::Node *skins = dtb::find(*theirStore, "skin"))
            for (size_t i = 1u; i < skins->nodes.size(); ++i)
                finishPrices[bodyOfTheirSkin[keyOf(skins->nodes[i])]].push_back(priceOf(skins->nodes[i]));
        std::map<std::string, std::string> bodyOfSkin; // GH2's
        std::erase_if(guitars->nodes, [&](const dtb::Node &guitar)
                      {
                          const dtb::Node *type = dtb::find(guitar, "type");
                          const bool isGuitar = type && type->nodes.size() > 1u && type->nodes[1].text == "guitar";
                          return guitar.type == dtb::kArray && isGuitar && bodies.count(keyOf(guitar)) == 0u;
                      });
        for (const dtb::Node &guitar : guitars->nodes)
            if (const dtb::Node *skins = dtb::find(guitar, "skins"))
                for (size_t i = 1u; i < skins->nodes.size(); ++i)
                    bodyOfSkin[keyOf(skins->nodes[i])] = keyOf(guitar);

        // Store.
        dtb::Node store = array({});
        dtb::Node list = array({symbol("guitar")});
        if (const dtb::Node *sold = dtb::find(*theirStore, "guitar"))
            for (size_t i = 1u; i < sold->nodes.size(); ++i)
                list.nodes.push_back(priced(body(keyOf(sold->nodes[i])), priceOf(sold->nodes[i])));
        store.nodes.push_back(list);
        list = array({symbol("skin")});
        std::map<std::string, size_t> taken;
        if (const dtb::Node *sold = dtb::find(*gameStore, "skin"))
            for (size_t i = 1u; i < sold->nodes.size(); ++i)
            {
                const auto owner = bodyOfSkin.find(keyOf(sold->nodes[i]));
                if (owner == bodyOfSkin.end())
                    continue;
                const std::vector<int32_t> &prices = finishPrices[owner->second];
                if (prices.empty())
                    continue;
                const size_t n = std::min(taken[owner->second]++, prices.size() - 1u);
                list.nodes.push_back(priced(keyOf(sold->nodes[i]), prices[n]));
            }
        store.nodes.push_back(list);
        if (const dtb::Node *sold = dtb::find(*theirStore, "song"))
            store.nodes.push_back(*sold);
        // GH1's three videos are not sold until one has been seen to play.
        store.nodes.push_back(array({symbol("video")}));
        list = array({symbol("character")});
        std::map<std::string, std::string> characters; // GH1's folder to GH2's character
        for (const Guitarist &g : guitarists)
            characters[g.folder] = g.character;
        dtb::Node soldCharacters = array({});
        if (const dtb::Node *sold = dtb::find(*theirStore, "character"))
            for (size_t i = 1u; i < sold->nodes.size(); ++i)
                if (const auto it = characters.find(keyOf(sold->nodes[i])); it != characters.end())
                {
                    list.nodes.push_back(priced(it->second, priceOf(sold->nodes[i])));
                    soldCharacters.nodes.push_back(symbol(it->second));
                }
        store.nodes.push_back(list);
        store.nodes.push_back(array({symbol("outfit")}));

        // Tips: GH1's are the general ones.
        if (dtb::Node *general = child(*tips, "tips_general"))
        {
            general->nodes.resize(1u);
            general->nodes.insert(general->nodes.end(), theirTips->nodes.begin(), theirTips->nodes.end());
        }

        // Characters, in the root config and the UI's lists, in the order
        // GH1's hero screen steps through them (its career.dta's
        // navigator): its first two are the players' defaults (main.dta).
        static const char *const kOrder[] = {"metal", "classic", "alterna", "hair_metal",
                                             "punk",  "nu_metal", "hiphop",  "grim"};
        std::vector<Guitarist> ordered;
        for (const char *folder : kOrder)
            for (const Guitarist &g : guitarists)
                if (std::string(g.folder) == folder)
                    ordered.push_back(g);
        dtb::Node config = array({symbol("characters")}), all = array({}), outfits = array({});
        std::string first, second;
        for (const Guitarist &g : ordered)
        {
            config.nodes.push_back(array({symbol(g.character), array({symbol(g.name())})}));
            all.nodes.push_back(symbol(g.character));
            outfits.nodes.push_back(symbol(g.name()));
            names[g.folder] = g.character;
            if (first.empty())
                first = g.name();
            else if (second.empty())
                second = g.name();
        }
        if (dtb::Node *n = child(*root, "characters"))
            *n = config;
        if (dtb::Node *n = child(*root, "venues"))
            *n = venues;
        define(*ui, "CHARACTERS", array({all}));
        define(*ui, "LOAD_CHARACTERS", array({outfits}));
        define(*ui, "STORE_CHARACTERS", soldCharacters);
        define(*ui, "STORE_OUTFITS", array({}));

        // Text: GH1's for what it names, by GH2's names for them.
        std::map<std::string, std::string> text;
        std::set<std::string> songs;
        if (const dtb::Node *sold = dtb::find(*theirStore, "song"))
            for (size_t i = 1u; i < sold->nodes.size(); ++i)
                songs.insert(keyOf(sold->nodes[i]));
        for (const dtb::Node &entry : strings->nodes)
        {
            if (entry.type != dtb::kArray || entry.nodes.size() < 2u)
                continue;
            const std::string key = keyOf(entry);
            const std::string stem = key.substr(0, key.rfind("_shop_desc") == std::string::npos ? key.size() : key.rfind("_shop_desc"));
            // The tips, the videos, the store's songs, its headings and each
            // category's blurb keep their names.
            static const std::set<std::string> kStore = {"category_cost", "guitar_shop_desc", "skin_shop_desc",
                                                         "song_shop_desc", "character_shop_desc"};
            const auto starts = [&](const char *with) { return key.rfind(with, 0) == 0; };
            if (const auto as = renamed(key, names))
                text[*as] = entry.nodes[1].text;
            else if (starts("loading_tip") || starts("video") || starts("store_") || songs.count(stem) != 0u ||
                     kStore.count(key) != 0u)
                text[key] = entry.nodes[1].text;
            // What the store says of a guitar, by GH2's name for that body.
            else if (stem != key && dtb::find(*theirGuitars, stem) != nullptr && bodies.count(body(stem)) != 0u)
                text[body(stem) + "_shop_desc"] = entry.nodes[1].text;
        }
        const size_t layer = ark::addLayer(std::nullopt);
        // GH1's intro movie, in its archive, where GH2 plays its own
        // (splash.dta's CUT_SCENE_VIDEO in each).
        ark::lend(layer, "videos/intro.pss", disc, "videos/ghintro.pss");
        const std::set<std::string> scenes = addMenus(layer, disc, text);
        for (dtb::Node &entry : locale->nodes)
            if (const auto it = text.find(keyOf(entry)); entry.type == dtb::kArray && entry.nodes.size() > 1u && it != text.end())
            {
                entry.nodes[1] = {dtb::kString, 0, 0.0f, it->second, {}};
                text.erase(it);
            }
        for (const auto &[key, value] : text)
            locale->nodes.push_back(array({symbol(key), {dtb::kString, 0, 0.0f, value, {}}}));

        ark::addFile(layer, "config/gen/gh2.dtb", dtb::write(*root));
        ark::addFile(layer, "config/gen/campaign.dtb", dtb::write(*campaign));
        ark::addFile(layer, "config/gen/store.dtb", dtb::write(store));
        ark::addFile(layer, "config/gen/guitars.dtb", dtb::write(*guitars));
        ark::addFile(layer, "config/gen/tips.dtb", dtb::write(*tips));
        // GH1's credits, which the credits screen reads as it opens
        // (CreditsPanel::Load, 0x143bd8).
        if (const auto credits = dtb::read("config/credits.dta", none, theirs))
            ark::addFile(layer, "config/gen/credits.dtb", dtb::write(*credits));
        // Menu music: GH1's loops, which its disc has under their own names
        // (sfx/streams), streamed as GH1 plays them: they are twice the size
        // of the loops GH2 holds in memory.
        auto synth = raw("config/gen/synth.dtb");
        const auto theirSynth = dtb::read("config/synth.dta", none, theirs);
        const dtb::Node *theirMusic = theirSynth ? dtb::find(*theirSynth, "metamusic") : nullptr;
        const dtb::Node *loops = theirMusic ? dtb::find(*theirMusic, "music") : nullptr;
        if (dtb::Node *music = synth ? child(*synth, "metamusic") : nullptr; music && loops)
        {
            if (dtb::Node *n = child(*music, "music"))
                *n = *loops;
            if (dtb::Node *n = child(*music, "play_from_memory"); n && n->nodes.size() > 1u)
                n->nodes[1] = {dtb::kInt, 0, 0.0f, {}, {}};
            ark::addFile(layer, "config/gen/synth.dtb", dtb::write(*synth));
        }
        ark::addFile(layer, "ui/gen/ui.dtb", dtb::write(*ui));
        ark::addFile(layer, "ui/eng/gen/locale.dtb", dtb::write(*locale));
        // GH2's screens' scripts (ui/init.dta includes each), fitted to GH1's
        // screens, and with the outfits they name: the main menu's two
        // defaults and the difficulty screen's guitarist.
        if (const auto init = raw("ui/gen/init.dtb"))
            for (const dtb::Node &include : init->nodes)
            {
                if (include.type != dtb::kInclude)
                    continue;
                const std::string name = include.text.substr(0, include.text.rfind('.'));
                const std::string script = "ui/gen/" + name + ".dtb";
                auto file = raw(script);
                if (!file)
                    continue;
                const bool fitted = fitScript(*file, scenes);
                if (name == "main" || name == "career")
                {
                    replaceSymbol(*file, "punk1", first);
                    replaceSymbol(*file, "rockabill1", second.empty() ? first : second);
                }
                else if (!fitted)
                    continue;
                ark::addFile(layer, script, dtb::write(*file));
            }
        // The store shows a character it sells by the character's name, as
        // GH2's sold characters' outfits are named: the guitarist again.
        const dtb::Node *sold = dtb::find(store, "character");
        for (const Guitarist &g : guitarists)
            if (sold && dtb::find(*sold, g.character))
                for (const char *suffix : {"", "_ui"})
                    if (auto file = ark::readFile("char/" + g.name() + "/og/gen/" + g.name() + suffix + ".milo_ps2"))
                        ark::addFile(layer, std::string("char/") + g.character + "/og/gen/" + g.character + suffix + ".milo_ps2",
                                     std::move(*file));
        addVenues(layer, disc);
        campaigns::add("gh1", layer);
    }
}
