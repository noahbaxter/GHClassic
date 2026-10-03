// 360 GH2's songs.dta entries are PS2 GH2's form, and its charts and lip
// sync files PS2's formats; only its audio differs (content/mogg.h). Every
// song shares a name with PS2's or might, and 51 of the 64 shared charts
// differ (22 in their notes), so each goes in as x_<name>, its files under
// songs/x_<name>/. The 360 has no practice audio (it slows the song itself),
// so its entries drop their practice keys.

#include "gh2x/install.h"

#include "content/games.h"
#include "content/locale.h"
#include "content/mogg.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "disc/ark.h"
#include "formats/dtb.h"

#include <iostream>
#include <string>

namespace gh2
{
    namespace
    {
        std::string ours(const std::string &name) { return "x_" + name; }

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
            }
            tiers.push_back(std::move(tier));
        }
        std::array<std::string, 5> scoreNames;
        for (size_t i = 0; i < scoreNames.size(); ++i)
            scoreNames[i] = text("highscore_dummy_" + std::to_string(i));
        setlists::add("gh2x", std::move(tiers), "ui/sel_song_quickplay.milo", scoreNames);
        std::cerr << "[gh2x] " << count << " songs" << std::endl;
    }
}
