#include "content/setlists.h"

#include "content/campaigns.h"
#include "content/locale.h"
#include "content/songs.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "guest.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <set>

namespace gh2::setlists
{
    namespace
    {
        struct Setlist
        {
            std::vector<Tier> tiers; // none for GH2's, which the game lists
            std::string look;
            Required required;
        };

        constexpr const char *kOwnLook = "ui/sel_song_quickplay.milo";
        constexpr int kBands = 8; // the save's band slots

        const Addresses *s_addresses = nullptr;
        std::map<std::string, Setlist> s_setlists;
        std::string s_selected = "gh2";
        std::string s_campaign = "gh2"; // the game played when it was picked

        // The setlist shown: the one picked, or after a campaign switch the
        // game now played's own.
        const std::string &selected()
        {
            if (s_campaign != campaigns::active())
                s_selected = s_campaign = campaigns::active();
            return s_selected;
        }
        std::map<std::string, uint32_t> s_symbols;

        // What the song list shows, when it is ours: each row's tier, and
        // whether it can be picked.
        const Setlist *s_shown = nullptr;
        std::vector<size_t> s_rowTiers;
        std::vector<bool> s_rowActive;

        PS2Runtime::RecompiledFunction s_initData = nullptr;
        PS2Runtime::RecompiledFunction s_gapSize = nullptr;
        PS2Runtime::RecompiledFunction s_isActive = nullptr;
        PS2Runtime::RecompiledFunction s_venueForSong = nullptr;

        uint32_t symbol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text)
        {
            const auto it = s_symbols.find(text);
            if (it != s_symbols.end())
                return it->second;
            return s_symbols[text] = script::symbol(rdram, ctx, runtime, text);
        }

        // The selected setlist when the provider fills quickplay's list
        // (its +0x30, set_quickplay, is also set for practice and
        // multiplayer) and it is not GH2's.
        const Setlist *ours(uint8_t *rdram, uint32_t provider)
        {
            if (selected() == campaigns::active() || load<uint32_t>(rdram, provider + 0x30u) == 0u)
                return nullptr;
            // GameConfig's mode Symbol, +0x3c (SongProvider::GetHighScore, 0x117b38).
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            const uint32_t mode = config ? load<uint32_t>(rdram, config + 0x3cu) : 0u;
            if (mode == 0u || std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), "quickplay") != 0)
                return nullptr;
            const auto it = s_setlists.find(selected());
            return it == s_setlists.end() ? nullptr : &it->second;
        }

        // SongProvider::InitData(RndDir *) (0x117448) fills its songs
        // (+0x4c, a vector of each song's config array) from the campaign
        // and its headers (+0x38, {first row, Symbol} each, shown as
        // song_header_<Symbol>). Ours fills both from the setlist, and the
        // last row to +0x48, as it does with no bonus songs.
        void initData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t provider = GPR_U32(ctx, 4);
            s_shown = ours(rdram, provider);
            s_rowTiers.clear();
            s_rowActive.clear();
            if (!s_shown)
                return s_initData(rdram, ctx, runtime);
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t scratch = sp - 0x20u;
            SET_GPR_U32(ctx, 29, scratch - 0x10u);
            store<uint32_t>(rdram, provider + 0x5cu, GPR_U32(ctx, 5));
            const uint32_t songs = provider + 0x4cu, headers = provider + 0x38u;
            store<uint32_t>(rdram, songs + 4u, load<uint32_t>(rdram, songs));
            store<uint32_t>(rdram, headers + 4u, load<uint32_t>(rdram, headers));
            // As Campaign::GetQuickplaySongs (0x12db78): the first two
            // tiers, and every song a band has unlocked in the game's career
            // on any difficulty, which the save's sections say for a game
            // not being played: its later tiers, its encores (each venue's
            // last song, CampaignState::GetEncoreForVenue 0x132ce0) and the
            // store songs bought. All access (+0x80, the unlock-all cheat)
            // opens every one. A game with no career yet has its store's
            // songs open.
            const uint32_t campaign = load<uint32_t>(rdram, s_addresses->theCampaign);
            const bool allAccess = campaign != 0u && load<uint32_t>(rdram, campaign + 0x80u) != 0u;
            const std::vector<std::string> careers = campaigns::games();
            const bool career = std::find(careers.begin(), careers.end(), selected()) != careers.end();
            const std::set<std::string> unlocked = save::unlockedSongs(selected());
            // A venue unlocked marks its encore unlocked with the rest. The
            // encore opens once a band has passed one short of the songs
            // the venue asks for on a difficulty (IsEncoreUnlocked,
            // 0x132c10; GetRequiredSongs, 0x1313d8).
            std::vector<bool> encoreOpen(s_shown->tiers.size(), false);
            for (int band = 0; band < kBands && !s_shown->required.empty(); ++band)
            {
                const auto passed = save::passedSongs(selected(), band);
                for (size_t d = 0; d < passed.size() && d < s_shown->required.size(); ++d)
                    for (size_t t = 0; t < s_shown->tiers.size(); ++t)
                    {
                        const Tier &tier = s_shown->tiers[t];
                        const bool last = t + 1u == s_shown->tiers.size() || !s_shown->tiers[t + 1u].encore;
                        int count = 0;
                        for (const std::string &song : tier.songs)
                            count += static_cast<int>(passed[d].count(song));
                        if (tier.encore && count >= s_shown->required[d][last ? 1 : 0] - 1)
                            encoreOpen[t] = true;
                    }
            }
            const auto open = [&](size_t t, size_t s)
            {
                const Tier &tier = s_shown->tiers[t];
                if (allAccess)
                    return true;
                if (tier.encore && s + 1u == tier.songs.size() && !s_shown->required.empty())
                    return static_cast<bool>(encoreOpen[t]);
                if (unlocked.count(tier.songs[s]) != 0u)
                    return true;
                if (!tier.encore)
                    return !career;
                return t < 2u && s + 1u < tier.songs.size();
            };
            for (size_t t = 0; t < s_shown->tiers.size(); ++t)
            {
                const Tier &tier = s_shown->tiers[t];
                bool any = false;
                for (size_t s = 0; s < tier.songs.size(); ++s)
                    any = any || open(t, s);
                if (!any)
                    continue;
                const uint32_t header[2] = {static_cast<uint32_t>(s_rowTiers.size()),
                                            symbol(rdram, ctx, runtime, tier.header)};
                pushBack(rdram, ctx, runtime, headers, header, 8u, s_addresses->headersInsertOverflow, scratch);
                for (size_t s = 0; s < tier.songs.size(); ++s)
                {
                    const uint32_t data = static_cast<uint32_t>(
                        runtime->callGuestFunction(rdram, ctx, s_addresses->songProviderGetSongData,
                                                   {provider, symbol(rdram, ctx, runtime, tier.songs[s])}));
                    pushBack(rdram, ctx, runtime, songs, &data, 4u, s_addresses->songsInsertOverflow, scratch);
                    s_rowTiers.push_back(t);
                    s_rowActive.push_back(open(t, s));
                }
            }
            store<int32_t>(rdram, provider + 0x48u, static_cast<int32_t>(s_rowTiers.size()) - 1);
            SET_GPR_U32(ctx, 29, sp);
            ctx->pc = returnTo;
        }

        // SongProvider::GapSize(int row) (0x1181d8): the space after a row,
        // 40 where the next row starts a tier.
        void gapSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_shown)
                return s_gapSize(rdram, ctx, runtime);
            const size_t row = static_cast<size_t>(static_cast<int32_t>(GPR_U32(ctx, 5)));
            const bool newTier = row + 1u < s_rowTiers.size() && s_rowTiers[row] != s_rowTiers[row + 1u];
            ctx->f[0] = newTier ? 40.0f : 0.0f;
            ctx->pc = GPR_U32(ctx, 31);
        }

        // SongProvider::IsActive(int row) (0x117a50): an encore only once the
        // campaign unlocks it; an inactive row shows blank.
        void isActive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_shown)
                return s_isActive(rdram, ctx, runtime);
            const size_t row = static_cast<size_t>(static_cast<int32_t>(GPR_U32(ctx, 5)));
            SET_GPR_U32(ctx, 2, row < s_rowActive.size() && s_rowActive[row] ? 1u : 0u);
            ctx->pc = GPR_U32(ctx, 31);
        }

        // CampaignData::GetVenueForSong(Symbol) (0x1312a0): the venue of the
        // song's tier in GH2's campaign, else the empty Symbol. A song whose
        // quickplay venue is locked plays there (GameConfig::SetQuickplay,
        // 0x126828), so a setlist's song gets its own tier's.
        void venueForSong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const std::string song = reinterpret_cast<const char *>(getMemPtr(rdram, GPR_U32(ctx, 5)));
            uint32_t venue = static_cast<uint32_t>(
                runtime->callGuestFunction(rdram, ctx, s_addresses->campaignDataGetVenueForSong,
                                           {GPR_U32(ctx, 4), GPR_U32(ctx, 5)}, s_venueForSong));
            if (load<uint8_t>(rdram, venue) == 0u)
            {
                // Its tier's venue, or where the game played has none of
                // that name (the 80s lack GH2's big and stone), the venue as
                // far into its own list.
                const std::vector<std::string> venues = campaigns::venues();
                for (const auto &[name, setlist] : s_setlists)
                    for (size_t t = 0; t < setlist.tiers.size(); ++t)
                    {
                        const Tier &tier = setlist.tiers[t];
                        if (std::find(tier.songs.begin(), tier.songs.end(), song) == tier.songs.end())
                            continue;
                        const bool here = venues.empty() ||
                                          std::find(venues.begin(), venues.end(), tier.venue) != venues.end();
                        venue = symbol(rdram, ctx, runtime,
                                       here ? tier.venue : venues[std::min(t, venues.size() - 1u)]);
                    }
            }
            SET_GPR_U32(ctx, 2, venue);
            ctx->pc = returnTo;
        }

        script::Node setlistCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "select")
            {
                const std::string name = call.symbol(2);
                const bool changed = s_setlists.count(name) != 0u && name != selected();
                if (changed)
                    s_selected = name;
                return {changed ? 1u : 0u, script::kInt};
            }
            // The game played's own list is in its own scene, at the game's
            // path for it.
            if (op == "look")
                return {symbol(call.rdram, call.ctx, call.runtime,
                               selected() == campaigns::active() ? kOwnLook : s_setlists.at(selected()).look),
                        script::kSymbol};
            if (op == "shown")
                return {symbol(call.rdram, call.ctx, call.runtime, selected()), script::kSymbol};
            std::cerr << "[setlist] usage: {setlist select <name>}, {setlist look|shown}" << std::endl;
            return {};
        }
    }

    void add(const std::string &name, std::vector<Tier> tiers, const std::string &look,
             const std::array<std::string, 5> &scoreNames, Required required)
    {
        std::vector<std::string> songs;
        for (const Tier &tier : tiers)
            songs.insert(songs.end(), tier.songs.begin(), tier.songs.end());
        save::addScoreSongs(name, songs, scoreNames);
        s_setlists[name] = {std::move(tiers), look, std::move(required)};
    }

    Required required(const dtb::Node &campaign)
    {
        Required out;
        if (const dtb::Node *counts = dtb::find(campaign, "required_songs"))
            for (size_t d = 1u; d < counts->nodes.size(); ++d)
                if (counts->nodes[d].nodes.size() > 2u)
                    out.push_back({counts->nodes[d].nodes[1].integer, counts->nodes[d].nodes[2].integer});
        return out;
    }

    std::vector<std::string> careerSongs(const std::string &name)
    {
        std::vector<std::string> songs;
        if (const auto it = s_setlists.find(name); it != s_setlists.end())
            for (const Tier &tier : it->second.tiers)
                if (tier.encore)
                    songs.insert(songs.end(), tier.songs.begin(), tier.songs.end());
        return songs;
    }

    void addDisc(const std::string &game, size_t disc, bool inConfig)
    {
        const dtb::Files files = [disc](const std::string &path) { return ark::readFile(disc, path); };
        dtb::Macros macros;
        const auto campaign = dtb::read("config/campaign.dta", macros, files);
        const auto songs = dtb::read("config/songs.dta", macros, files);
        const auto store = dtb::read("config/store.dta", macros, files);
        const auto strings = dtb::read("ui/eng/locale.dta", macros, files);
        const dtb::Node *order = campaign ? dtb::find(*campaign, "order") : nullptr;
        const dtb::Node *bonus = store ? dtb::find(*store, "song") : nullptr;
        if (!order || !songs || !strings)
        {
            std::cerr << "[" << game << "] cannot read its campaign" << std::endl;
            return;
        }
        const auto text = [&](const std::string &token) -> std::string
        {
            const dtb::Node *found = dtb::find(*strings, token);
            return found && found->nodes.size() > 1u ? found->nodes[1].text : "";
        };
        // The career's tiers (campaign.dta's order), then the store's songs,
        // each headed as the game heads it.
        std::vector<Tier> tiers;
        for (size_t i = 1u; i <= order->nodes.size(); ++i)
        {
            const bool isStore = i == order->nodes.size();
            if (isStore && (!bonus || bonus->nodes.size() < 2u || tiers.empty()))
                break;
            const dtb::Node &list = isStore ? *bonus : order->nodes[i];
            if (list.nodes.empty())
                continue;
            const std::string group = isStore ? "store" : list.nodes[0].text;
            Tier tier{isStore ? tiers.front().venue : group, game + "_" + group, {}, !isStore};
            locale::add("song_header_" + tier.header, text("song_header_" + group));
            for (size_t s = 1u; s < list.nodes.size(); ++s)
            {
                const dtb::Node &item = list.nodes[s];
                const std::string name = item.type == dtb::kArray && !item.nodes.empty() ? item.nodes[0].text : item.text;
                const dtb::Node *entry = dtb::find(*songs, name);
                if (!entry)
                    continue;
                if (!inConfig)
                {
                    songs::add(dtb::text(*entry));
                    ark::rename("songs/" + name + "/", disc, "songs/" + name + "/");
                }
                tier.songs.push_back(name);
            }
            tiers.push_back(std::move(tier));
        }
        // Its archive whole under <game>/, so its song list scene finds what
        // it refers to on its own disc.
        ark::rename(game + "/", disc, "");
        std::array<std::string, 5> scoreNames;
        for (size_t i = 0; i < scoreNames.size(); ++i)
            scoreNames[i] = text("highscore_dummy_" + std::to_string(i));
        add(game, std::move(tiers), game + "/" + kOwnLook, scoreNames, required(*campaign));
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        addDisc("gh2", 0u, true);
        script::addCommand("setlist", setlistCommand);
        s_initData = runtime.lookupFunction(addresses.songProviderInitData);
        runtime.replaceFunction(addresses.songProviderInitData, &initData);
        s_gapSize = runtime.lookupFunction(addresses.songProviderGapSize);
        runtime.replaceFunction(addresses.songProviderGapSize, &gapSize);
        s_isActive = runtime.lookupFunction(addresses.songProviderIsActive);
        runtime.replaceFunction(addresses.songProviderIsActive, &isActive);
        s_venueForSong = runtime.lookupFunction(addresses.campaignDataGetVenueForSong);
        runtime.replaceFunction(addresses.campaignDataGetVenueForSong, &venueForSong);
    }
}
