#include "content/setlists.h"

#include "guest.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>

namespace gh2::setlists
{
    namespace
    {
        struct Setlist
        {
            std::vector<Tier> tiers; // none for GH2's, which the game lists
            std::string look;
        };

        const Addresses *s_addresses = nullptr;
        std::map<std::string, Setlist> s_setlists;
        std::string s_selected = "gh2";
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
            if (s_selected == "gh2" || load<uint32_t>(rdram, provider + 0x30u) == 0u)
                return nullptr;
            // GameConfig's mode Symbol, +0x3c (SongProvider::GetHighScore, 0x117b38).
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            const uint32_t mode = config ? load<uint32_t>(rdram, config + 0x3cu) : 0u;
            if (mode == 0u || std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), "quickplay") != 0)
                return nullptr;
            const auto it = s_setlists.find(s_selected);
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
            // tiers, or every one with all access (+0x80, the unlock-all
            // cheat). Career progress opens the rest and the encores (each
            // venue's last song, CampaignState::GetEncoreForVenue 0x132ce0),
            // and no setlist but GH2's has a career yet.
            const uint32_t campaign = load<uint32_t>(rdram, s_addresses->theCampaign);
            const bool allAccess = campaign != 0u && load<uint32_t>(rdram, campaign + 0x80u) != 0u;
            const size_t open = allAccess ? s_shown->tiers.size() : std::min<size_t>(2u, s_shown->tiers.size());
            for (size_t t = 0; t < open; ++t)
            {
                const Tier &tier = s_shown->tiers[t];
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
                    s_rowActive.push_back(allAccess || !tier.encore || s + 1u < tier.songs.size());
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
                for (const auto &[name, setlist] : s_setlists)
                    for (const Tier &tier : setlist.tiers)
                        if (std::find(tier.songs.begin(), tier.songs.end(), song) != tier.songs.end())
                            venue = symbol(rdram, ctx, runtime, tier.venue);
            SET_GPR_U32(ctx, 2, venue);
            ctx->pc = returnTo;
        }

        script::Node setlistCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "select")
            {
                const std::string name = call.symbol(2);
                const bool changed = s_setlists.count(name) != 0u && name != s_selected;
                if (changed)
                    s_selected = name;
                return {changed ? 1u : 0u, script::kInt};
            }
            if (op == "look")
                return {symbol(call.rdram, call.ctx, call.runtime, s_setlists.at(s_selected).look), script::kSymbol};
            std::cerr << "[setlist] usage: {setlist select <name>}, {setlist look}" << std::endl;
            return {};
        }
    }

    void add(const std::string &name, std::vector<Tier> tiers, const std::string &look,
             const std::array<std::string, 5> &scoreNames)
    {
        std::vector<std::string> songs;
        for (const Tier &tier : tiers)
            songs.insert(songs.end(), tier.songs.begin(), tier.songs.end());
        save::addScoreSongs(name, songs, scoreNames);
        s_setlists[name] = {std::move(tiers), look};
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        s_setlists.emplace("gh2", Setlist{{}, "ui/sel_song_quickplay.milo"});
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
