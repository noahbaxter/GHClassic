// 360 GH2's songs.dta entries are PS2 GH2's form, and its charts and lip
// sync files PS2's formats; only its audio differs (content/mogg.h). Every
// song shares a name with PS2's or might, and 51 of the 64 shared charts
// differ (22 in their notes), so each goes in as x_<name>, its files under
// songs/x_<name>/. The 360 has no practice audio (it slows the song itself),
// so its entries drop their practice keys.
//
// Its career is a campaign (content/campaigns.h). The 360's archive is in
// Xbox formats, so nothing of it can stand in front: the campaign is the game
// disc with the 360's campaign, store, guitar requirements and tips over
// it, each made here in GH2's form from the 360's own, its songs by our
// names. The models, venues and screens are PS2's.

#include "gh2x/install.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "content/hidden_guitars.h"
#include "content/locale.h"
#include "content/mogg.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh2x/focus.h"
#include "gh2x/models.h"
#include "gh2x/shadows.h"
#include "gh2x/textures.h"
#include "gh2x/track.h"
#include "ps2_runtime.h"
#include "render/texture_store.h"
#include "settings/settings.h"

#include <algorithm>
#include <functional>
#include <iostream>
#include <string>

namespace gh2
{
    namespace
    {
        std::string ours(const std::string &name) { return "x_" + name; }

        // A side of the PS2 store's album art (its us_logo pictures).
        constexpr uint32_t kAlbumArtSide = 256u;
        constexpr const char *kSongsToContinue = "song_current_venue";
        constexpr const char *kGamePanel = "ui/gen/game.dtb";

        // Paths under songs/<name>/ moved to songs/x_<name>/, practice keys
        // dropped.
        void retarget(dtb::Node &node, const std::string &name)
        {
            const std::string from = "songs/" + name + "/", to = "songs/" + ours(name) + "/";
            if ((node.type == dtb::kSymbol || node.type == dtb::kString) && node.text.rfind(from, 0) == 0)
                node.text = to + node.text.substr(from.size());
            std::erase_if(node.nodes, [](const dtb::Node &n)
                          {
                              const std::string key = n.type == dtb::kArray && !n.nodes.empty() ? n.nodes[0].text : "";
                              return key == "practice_speeds" || key.rfind("song_practice_", 0) == 0;
                          });
            for (dtb::Node &n : node.nodes)
                retarget(n, name);
        }

        // One entry in as x_<name>: its chart and lip sync read from the 360
        // disc, each audio (song, song_coop) made from its mogg.
        bool addSong(size_t disc, const dtb::Node &songs, const std::string &name)
        {
            const dtb::Node *found = dtb::find(songs, name);
            if (!found)
                return false;
            dtb::Node entry = *found;
            entry.nodes[0].text = ours(name);
            retarget(entry, name);
            for (const char *key : {"song", "song_coop"})
            {
                const dtb::Node *audio = dtb::find(*found, key);
                const dtb::Node *path = audio ? dtb::find(*audio, "name") : nullptr;
                if (!path || path->nodes.size() < 2u)
                    continue;
                const std::string mogg = path->nodes[1].text;
                const std::string file = mogg.substr(mogg.rfind('/') + 1u);
                mogg::serveAsVgs("songs/" + ours(name) + "/" + file + ".vgs", disc, mogg + ".mogg");
            }
            songs::add(dtb::text(entry));
            // The singer's lip sync goes by the song's name, not its entry.
            const std::string dir = "songs/" + ours(name) + "/";
            ark::rename(dir + ours(name) + ".voc", disc, "songs/" + name + "/" + name + ".voc");
            ark::rename(dir, disc, "songs/" + name + "/");
            return true;
        }

        std::string keyOf(const dtb::Node &node)
        {
            return node.type == dtb::kArray && !node.nodes.empty() ? node.nodes[0].text : node.text;
        }

        dtb::Node *child(dtb::Node &array, const std::string &key)
        {
            for (dtb::Node &n : array.nodes)
                if (n.type == dtb::kArray && keyOf(n) == key)
                    return &n;
            return nullptr;
        }

        // Every song a list names, by our name: (battle surrender ...) or
        // ((rawdog (price 400)) ...).
        void rename(dtb::Node &list)
        {
            for (size_t i = 1u; i < list.nodes.size(); ++i)
            {
                dtb::Node &item = list.nodes[i];
                std::string &name = item.type == dtb::kArray && !item.nodes.empty() ? item.nodes[0].text : item.text;
                name = ours(name);
            }
        }

        // PS2's guitars, which name PS2's models, each asking what the
        // 360's of that name asks: (require (songs 10) (stars 3)), or
        // nothing.
        void requireAs(dtb::Node &guitars, const dtb::Node &theirs)
        {
            for (dtb::Node &guitar : guitars.nodes)
            {
                const dtb::Node *other = dtb::find(theirs, keyOf(guitar));
                if (guitar.type != dtb::kArray || !other)
                    continue;
                const dtb::Node *require = dtb::find(*other, "require");
                const auto mine = std::find_if(guitar.nodes.begin(), guitar.nodes.end(),
                                               [](const dtb::Node &n) { return keyOf(n) == "require" && n.type == dtb::kArray; });
                if (mine != guitar.nodes.end() && require)
                    *mine = *require;
                else if (mine != guitar.nodes.end())
                    guitar.nodes.erase(mine);
                else if (require)
                    guitar.nodes.insert(guitar.nodes.begin() + 1, *require);
            }
        }

        // The career's files, in a layer over the game disc.
        // The symbol a script compares `{object method}` to: battle in
        // {== {game get_venue} battle}.
        const dtb::Node *comparedTo(const dtb::Node &node, const char *object, const char *method)
        {
            if (node.nodes.size() != 3u || node.nodes[0].text != "==" || node.nodes[2].type != dtb::kSymbol)
                return nullptr;
            const dtb::Node &call = node.nodes[1];
            return call.nodes.size() == 2u && call.nodes[0].text == object && call.nodes[1].text == method ? &node.nodes[2]
                                                                                                           : nullptr;
        }

        // Every test in a script for one venue's one song, {&& {== {game
        // get_venue} battle} ... {== {gamecfg get_song} tonightimgonna}}:
        // game.dta's two, for the encore whose drummer explodes.
        void encoreTests(dtb::Node &node, std::vector<std::pair<dtb::Node *, dtb::Node *>> &out)
        {
            if (!node.nodes.empty() && node.nodes[0].text == "&&")
            {
                dtb::Node *venue = nullptr, *song = nullptr;
                const std::function<void(dtb::Node &)> look = [&](dtb::Node &n)
                {
                    if (const dtb::Node *v = comparedTo(n, "game", "get_venue"))
                        venue = const_cast<dtb::Node *>(v);
                    if (const dtb::Node *s = comparedTo(n, "gamecfg", "get_song"))
                        song = const_cast<dtb::Node *>(s);
                    for (dtb::Node &c : n.nodes)
                        look(c);
                };
                look(node);
                if (venue && song)
                {
                    out.emplace_back(venue, song);
                    return;
                }
            }
            for (dtb::Node &c : node.nodes)
                encoreTests(c, out);
        }

        void addCareer(size_t disc, dtb::Node campaign, const dtb::Node &strings)
        {
            const dtb::Files theirs = [disc](const std::string &path) { return ark::readFile(disc, path); };
            const dtb::Files game = [](const std::string &path) { return ark::readFile(0u, path); };
            // As a PS2 build reads them: the store's videos are the .pss.
            dtb::Macros ps2;
            ps2["HX_EE"] = {};
            auto store = dtb::read("config/store.dta", ps2, theirs);
            dtb::Macros none;
            auto guitars = dtb::read("config/guitars.dta", none, game);
            const auto theirGuitars = dtb::read("config/guitars.dta", none, theirs);
            auto tips = dtb::read("config/tips.dta", none, theirs);
            if (!store || !guitars || !theirGuitars || !tips)
            {
                std::cerr << "[gh2x] cannot read the 360 career" << std::endl;
                return;
            }
            if (dtb::Node *order = child(campaign, "order"))
                for (size_t i = 1u; i < order->nodes.size(); ++i)
                    rename(order->nodes[i]);
            if (dtb::Node *songs = child(*store, "song"))
                rename(*songs);
            requireAs(*guitars, *theirGuitars);
            offerHiddenGuitars(*guitars, *store, false);
            // loading_tip13 is about the Xbox Live leaderboards.
            for (dtb::Node &list : tips->nodes)
                std::erase_if(list.nodes, [](const dtb::Node &n) { return keyOf(n) == "loading_tip13"; });
            if (const dtb::Node *tip = dtb::find(strings, "loading_tip14"); tip && tip->nodes.size() > 1u)
                locale::add("loading_tip14", tip->nodes[1].text);

            // The setlist's "Beat %i/5 songs to continue": a 360 venue has
            // five songs before its encore where PS2's has four.
            auto text = dtb::read("ui/eng/locale.dta", none, game);
            const dtb::Node *theirLine = dtb::find(strings, kSongsToContinue);
            dtb::Node *line = text ? child(*text, kSongsToContinue) : nullptr;
            if (line && theirLine && line->nodes.size() > 1u && theirLine->nodes.size() > 1u)
                line->nodes[1].text = theirLine->nodes[1].text;

            // The encore whose drummer explodes at its end: PS2's game.dta
            // asks for Tonight I'm Gonna Rock You at battle, the 360's for
            // it at small1, where its career has it.
            const auto gamePanel = ark::readFile(0u, kGamePanel), theirGamePanel = ark::readFile(disc, kGamePanel);
            auto panel = gamePanel ? dtb::raw(*gamePanel) : std::nullopt;
            auto theirPanel = theirGamePanel ? dtb::raw(*theirGamePanel) : std::nullopt;
            std::vector<std::pair<dtb::Node *, dtb::Node *>> tests, theirTests;
            if (panel && theirPanel)
            {
                encoreTests(*panel, tests);
                encoreTests(*theirPanel, theirTests);
            }
            const bool drummer = !tests.empty() && tests.size() == theirTests.size();
            for (size_t i = 0; drummer && i < tests.size(); ++i)
            {
                tests[i].first->text = theirTests[i].first->text;
                tests[i].second->text = ours(theirTests[i].second->text);
            }

            const size_t layer = ark::addLayer(std::nullopt);
            if (line)
                ark::addFile(layer, "ui/eng/gen/locale.dtb", dtb::write(*text));
            if (drummer)
                ark::addFile(layer, kGamePanel, dtb::write(*panel));
            else
                std::cerr << "[gh2x] no encore test in " << kGamePanel << std::endl;
            ark::addFile(layer, "config/gen/campaign.dtb", dtb::write(campaign));
            ark::addFile(layer, "config/gen/store.dtb", dtb::write(*store));
            ark::addFile(layer, "config/gen/guitars.dtb", dtb::write(*guitars));
            ark::addFile(layer, "config/gen/tips.dtb", dtb::write(*tips));
            campaigns::add("gh2x", layer);
        }
    }

    void installGh2x()
    {
        const auto disc = games::disc("gh2x");
        if (!disc)
            return;
        const dtb::Files files = [disc](const std::string &path) { return ark::readFile(*disc, path); };
        dtb::Macros macros;
        const auto campaign = dtb::read("config/campaign.dta", macros, files);
        const auto songs = dtb::read("config/songs.dta", macros, files);
        const auto store = dtb::read("config/store.dta", macros, files);
        const auto strings = dtb::read("ui/eng/locale.dta", macros, files);
        const dtb::Node *order = campaign ? dtb::find(*campaign, "order") : nullptr;
        const dtb::Node *bonus = store ? dtb::find(*store, "song") : nullptr;
        if (!order || !songs || !bonus || !strings)
        {
            std::cerr << "[gh2x] cannot read the 360 campaign" << std::endl;
            return;
        }
        const auto text = [&](const std::string &token) -> std::string
        {
            const dtb::Node *found = dtb::find(*strings, token);
            return found && found->nodes.size() > 1u ? found->nodes[1].text : "";
        };

        // The career's tiers, then the store's songs, as the 360 heads them.
        std::vector<setlists::Tier> tiers;
        size_t count = 0u;
        const std::vector<std::string> logos = ark::paths(*disc, "ui/image/ng/gen/us_logo_");
        for (size_t i = 1u; i <= order->nodes.size(); ++i)
        {
            const bool isStore = i == order->nodes.size();
            const dtb::Node &list = isStore ? *bonus : order->nodes[i];
            if (list.nodes.empty())
                continue;
            const std::string group = isStore ? "store" : list.nodes[0].text;
            setlists::Tier tier{isStore ? tiers.front().venue : group, "gh2x_" + group, {}, !isStore};
            locale::add("song_header_" + tier.header, text("song_header_" + group));
            for (size_t s = 1u; s < list.nodes.size(); ++s)
            {
                const dtb::Node &item = list.nodes[s];
                const std::string name = item.type == dtb::kArray && !item.nodes.empty() ? item.nodes[0].text : item.text;
                if (!addSong(*disc, *songs, name))
                {
                    std::cerr << "[gh2x] no entry for " << name << std::endl;
                    continue;
                }
                tier.songs.push_back(ours(name));
                ++count;
                // The store's album art for it, asked for by our name: the
                // 360's picture, at the size the PS2's are.
                const std::string art = "ui/image/ng/gen/us_logo_" + name + "_keep.png_xbox";
                if (isStore && std::binary_search(logos.begin(), logos.end(), art))
                    ark::addMade("ui/image/og/gen/us_logo_" + ours(name) + "_keep.png_ps2",
                                 [xbox = *disc, art]() -> std::optional<ark::Made>
                                 {
                                     std::vector<uint8_t> standIn =
                                         textures::standIn(textures::add({xbox, art, {}}), kAlbumArtSide, kAlbumArtSide);
                                     return standIn.empty() ? std::nullopt : std::optional(ark::held(std::move(standIn)));
                                 });
                // The store's name and blurb for it, and its loading tip.
                for (const auto &[token, as] : {std::pair{name, ours(name)},
                                                std::pair{name + "_shop_desc", ours(name) + "_shop_desc"},
                                                std::pair{"loading_tip_" + name, "loading_tip_" + ours(name)}})
                    if (const std::string found = text(token); !found.empty())
                        locale::add(as, found);
            }
            tiers.push_back(std::move(tier));
        }
        std::array<std::string, 5> scoreNames;
        for (size_t i = 0; i < scoreNames.size(); ++i)
            scoreNames[i] = text("highscore_dummy_" + std::to_string(i));
        setlists::add("gh2x", std::move(tiers), "ui/sel_song_quickplay.milo", scoreNames, setlists::required(*campaign));
        addCareer(*disc, *campaign, *strings);
        std::cerr << "[gh2x] " << count << " songs" << std::endl;
    }

    void installGh2xAssets(PS2Runtime &runtime, const Addresses &addresses)
    {
        // The 360's look is its own songs' and career's (settings' authentic).
        uint8_t *rdram = runtime.memory().getRDRAM();
        settings::setNextGenGame(
            [rdram] { return (campaigns::inCareer(rdram) ? campaigns::active() : setlists::shown()) == "gh2x"; });
        installGh2xTextures();
        installGh2xModels();
        gh2x::installFocus(runtime, addresses);
        gh2x::installShadows(runtime, addresses);
        gh2x::installTrack(runtime, addresses);
    }
}
