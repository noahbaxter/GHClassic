// The 80s guitarists are GH2's characters in new outfits, built on the same
// rigs (every anim but goth1_main is byte-identical), and they sit at GH2's
// outfit paths. Each goes in under a name of its own as one more outfit of
// its character, served from the 80s disc and playing GH2's anims.
//
// An outfit loads char/<outfit>/og/<outfit>.milo (AddLoadChar, 0x128778),
// with _ui and _horse beside it.

#include "gh80s/install.h"

#include "disc/ark.h"
#include "content/locale.h"
#include "content/outfits.h"
#include "content/setlists.h"
#include "content/songs.h"
#include "formats/dtb.h"

#include <iostream>
#include <string>

namespace gh2
{
    namespace
    {
        struct Outfit
        {
            const char *character; // in config's (characters ...)
            const char *label;     // the outfit picker's (sel_character.dta)
            const char *base;      // GH2's outfit it sits beside, here replaced
            const char *name;      // ours
        };

        // The 80s disc names its outfits as GH2 does the ones they replaced,
        // so base is also the folder and file stem it is read from.
        constexpr Outfit kOutfits[] = {
            // character  label          base        name
            {"punk",      "SIDE SWEEP",  "punk1",    "punk3"},
            {"alterna",   "LACE",        "alterna1", "alterna3"},
            {"metal",     "JACKET",      "metal1",   "metal3"},
            {"glam",      "BANDANAS",    "glam1",    "glam3"},
            {"goth",      "BLAZERS",     "goth2",    "goth3"},
            {"grim",      "WATCHIN'",    "grim",     "grim2"},
        };

        // The 80s career's songs (campaign.dta's order), tier by tier, as
        // quickplay's 80s setlist. Its venues are GH2's names, so its tier
        // headers go in as gh80s_<venue>, the 80s locale's text.
        void addSetlist(size_t disc)
        {
            const dtb::Files files = [disc](const std::string &path) { return ark::readFile(disc, path); };
            dtb::Macros macros;
            const auto campaign = dtb::read("config/campaign.dta", macros, files);
            const auto songs = dtb::read("config/songs.dta", macros, files);
            const auto strings = dtb::read("ui/eng/locale.dta", macros, files);
            const dtb::Node *order = campaign ? dtb::find(*campaign, "order") : nullptr;
            if (!order || !songs || !strings)
            {
                std::cerr << "[gh80s] cannot read the 80s campaign" << std::endl;
                return;
            }
            std::vector<setlists::Tier> tiers;
            for (size_t i = 1u; i < order->nodes.size(); ++i)
            {
                const dtb::Node &venue = order->nodes[i];
                if (venue.nodes.empty())
                    continue;
                setlists::Tier tier{venue.nodes[0].text, "gh80s_" + venue.nodes[0].text, {}};
                if (const dtb::Node *header = dtb::find(*strings, "song_header_" + venue.nodes[0].text);
                    header && header->nodes.size() > 1u)
                    locale::add("song_header_" + tier.header, header->nodes[1].text);
                for (size_t s = 1u; s < venue.nodes.size(); ++s)
                {
                    const std::string &name = venue.nodes[s].text;
                    const dtb::Node *entry = dtb::find(*songs, name);
                    if (!entry)
                        continue;
                    songs::add(dtb::text(*entry));
                    ark::rename("songs/" + name + "/", disc, "songs/" + name + "/");
                    tier.songs.push_back(name);
                }
                tiers.push_back(std::move(tier));
            }
            // The 80s archive whole under gh80s/, so its song list scene
            // finds what it refers to on its own disc.
            ark::rename("gh80s/", disc, "");
            std::array<std::string, 5> scoreNames;
            for (size_t i = 0; i < scoreNames.size(); ++i)
                if (const dtb::Node *name = dtb::find(*strings, "highscore_dummy_" + std::to_string(i));
                    name && name->nodes.size() > 1u)
                    scoreNames[i] = name->nodes[1].text;
            setlists::add("gh80s", std::move(tiers), "gh80s/ui/sel_song_quickplay.milo", scoreNames);
        }
    }

    void installEighties()
    {
        const auto disc = ark::discWithSerial("SLUS_215.86");
        if (!disc)
            return;
        addSetlist(*disc);
        for (const Outfit &outfit : kOutfits)
        {
            const std::string base = outfit.base, name = outfit.name;
            ark::rename("char/" + name + "/og/gen/" + name, *disc, "char/" + base + "/og/gen/" + base);
            // The highway (track/surfaces/%s_keep.bmp) and photos go by
            // outfit too.
            ark::rename("track/surfaces/gen/" + name + "_keep", *disc, "track/surfaces/gen/" + base + "_keep");
            outfits::photosFrom(name, *disc, base);
            outfits::add(outfit.character, name, base, outfit.label);
        }
    }
}
