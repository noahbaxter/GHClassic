#include "content/setlists.h"

#include "content/campaigns.h"
#include "content/encores.h"
#include "content/locale.h"
#include "content/setlist_look.h"
#include "content/setlist_paper.h"
#include "content/setlist_window.h"
#include "content/songs.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "guest.h"
#include "milo/milo.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/host_clock.h"

#include <algorithm>
#include <array>
#include <chrono>
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
            Live live; // its rows, where they are kept elsewhere
            bool twoLines = false; // in rows of two lines, its Live's under the name
        };

        using namespace setlist_window;
        std::map<std::string, Window> s_windows;

        constexpr const char *kOwnLook = "ui/sel_song_quickplay.milo";
        constexpr int kBands = 8; // the save's band slots

        const Addresses *s_addresses = nullptr;
        std::map<std::string, Setlist> s_setlists;
        std::string s_selected = "gh2";
        std::string s_next; // picked, shown once the list it turns to enters
        std::string s_campaign = "gh2"; // the game played when it was picked

        // Whether there is such a setlist with songs to show.
        bool has(const std::string &name)
        {
            const auto it = s_setlists.find(name);
            return it != s_setlists.end() && (!it->second.live.rows || !it->second.live.rows()->empty());
        }

        // The setlist shown: the one picked, or after a campaign switch the
        // game now played's own. The first asked, the one picked last run
        // if its game is the one played. Kept in state.ini, [setlist].
        const std::string &selected()
        {
            static bool restored = false;
            if (!restored)
            {
                restored = true;
                const std::string shown = save::state("setlist", "shown");
                if (save::state("setlist", "game") == campaigns::active() && has(shown))
                {
                    s_campaign = campaigns::active();
                    s_selected = shown;
                }
            }
            if (s_campaign != campaigns::active())
            {
                s_selected = s_campaign = campaigns::active();
                save::keepState("setlist", "shown", s_selected);
                save::keepState("setlist", "game", s_campaign);
            }
            return s_selected;
        }
        std::map<std::string, uint32_t> s_symbols;
        std::set<std::string> s_zeroScores;
        uint32_t s_provider = 0u; // song_provider, as InitData last had it

        // What the song list shows, when it is ours: each row's tier, and
        // whether it can be picked.
        const Setlist *s_shown = nullptr;
        std::chrono::steady_clock::time_point s_filled; // when last filled
        std::vector<size_t> s_rowTiers;
        std::vector<bool> s_rowActive;

        PS2Runtime::RecompiledFunction s_initData = nullptr;
        PS2Runtime::RecompiledFunction s_gapSize = nullptr;
        PS2Runtime::RecompiledFunction s_isActive = nullptr;
        PS2Runtime::RecompiledFunction s_venueForSong = nullptr;
        PS2Runtime::RecompiledFunction s_getHighScore = nullptr;
        PS2Runtime::RecompiledFunction s_textSlotFill = nullptr;

        uint32_t symbol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text)
        {
            const auto it = s_symbols.find(text);
            if (it != s_symbols.end())
                return it->second;
            return s_symbols[text] = script::symbol(rdram, ctx, runtime, text);
        }

        // GameConfig's mode Symbol, +0x3c (SongProvider::GetHighScore, 0x117b38).
        bool modeIs(uint8_t *rdram, const char *name)
        {
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            const uint32_t mode = config ? load<uint32_t>(rdram, config + 0x3cu) : 0u;
            return mode != 0u && std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), name) == 0;
        }

        // The selected setlist when the provider fills quickplay's list
        // (its +0x30, set_quickplay, is also set for practice and
        // multiplayer), in quickplay, Face-Off and Pro Face-Off
        // (ui/dta/multiplayer.dta) and practice; co-op's is its own career's. The game
        // played's too: GH2's own list reads the Campaign, which holds only
        // the careers of the note engine judging.
        const Setlist *ours(uint8_t *rdram, uint32_t provider)
        {
            if (load<uint32_t>(rdram, provider + 0x30u) == 0u)
                return nullptr;
            if (!modeIs(rdram, "quickplay") && !modeIs(rdram, "multi_vs") && !modeIs(rdram, "multi_fo") &&
                !modeIs(rdram, "practice"))
                return nullptr;
            const auto it = s_setlists.find(selected());
            return it == s_setlists.end() ? nullptr : &it->second;
        }

        // A venue unlocked marks its encore unlocked with the rest. The
        // encore opens once a band has passed one short of the songs the
        // venue asks for on a difficulty (IsEncoreUnlocked, 0x132c10;
        // GetRequiredSongs, 0x1313d8): each tier's, whether it has.
        std::vector<bool> encoresOpen(const Setlist &shown)
        {
            std::vector<bool> open(shown.tiers.size(), false);
            for (int band = 0; band < kBands; ++band)
            {
                const auto passed = save::passedSongs(selected(), band);
                for (size_t d = 0; d < passed.size() && d < shown.required.size(); ++d)
                    for (size_t t = 0; t < shown.tiers.size(); ++t)
                    {
                        const Tier &tier = shown.tiers[t];
                        const bool last = t + 1u == shown.tiers.size() || !shown.tiers[t + 1u].encore;
                        int count = 0;
                        for (const std::string &song : tier.songs)
                            count += static_cast<int>(passed[d].count(song));
                        if (tier.encore && count >= shown.required[d][last ? 1 : 0] - 1)
                            open[t] = true;
                    }
            }
            return open;
        }

        // A setlist's tiers' rows, as Campaign::GetQuickplaySongs
        // (0x12db78): the first two tiers, and every song a band has
        // unlocked in the game's career on any difficulty, under either
        // note engine, as the save's sections say: its later tiers, its
        // encores (each venue's last song, CampaignState::GetEncoreForVenue
        // 0x132ce0) and the store songs bought. All access (+0x80, the
        // unlock-all cheat) opens every one. A game with no career yet has
        // its store's songs open. A game without encores has a tier's last
        // song open with the rest.
        void tierRows(uint8_t *rdram, Window &window, const Setlist &shown, std::vector<bool> &active)
        {
            const uint32_t campaign = load<uint32_t>(rdram, s_addresses->theCampaign);
            const bool allAccess = campaign != 0u && load<uint32_t>(rdram, campaign + 0x80u) != 0u;
            const std::vector<std::string> careers = campaigns::games();
            const bool career = std::find(careers.begin(), careers.end(), selected()) != careers.end();
            const std::set<std::string> unlocked = save::unlockedSongs(selected());
            const bool gated = encores::has(selected()) && !shown.required.empty();
            const std::vector<bool> encoreOpen = gated ? encoresOpen(shown) : std::vector<bool>(shown.tiers.size(), false);
            const auto open = [&](size_t t, size_t s)
            {
                const Tier &tier = shown.tiers[t];
                if (allAccess)
                    return true;
                if (gated && tier.encore && s + 1u == tier.songs.size())
                    return static_cast<bool>(encoreOpen[t]);
                if (unlocked.count(tier.songs[s]) != 0u)
                    return true;
                if (!tier.encore)
                    return !career;
                return t < 2u && (!gated || s + 1u < tier.songs.size());
            };
            for (size_t t = 0; t < shown.tiers.size(); ++t)
            {
                const Tier &tier = shown.tiers[t];
                bool any = false;
                for (size_t s = 0; s < tier.songs.size(); ++s)
                    any = any || open(t, s);
                for (size_t s = 0; s < tier.songs.size(); ++s)
                    // A song not open has no row, as the game's own list
                    // has none for a store song not bought: but an encore
                    // still to earn in a tier with songs open, whose row
                    // is there blank.
                    if (open(t, s) || (any && gated && tier.encore && s + 1u == tier.songs.size()))
                    {
                        window.tiers.push_back(t);
                        window.songs.push_back(tier.songs[s]);
                        active.push_back(open(t, s));
                    }
            }
        }

        // The window's rows into the provider's songs and headers, its
        // songs made ready first where the setlist is live. `scratch` is
        // 16 free guest bytes.
        void pushWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t provider, uint32_t scratch,
                        Window &window, const std::vector<bool> &active)
        {
            const uint32_t songs = provider + 0x4cu, headers = provider + 0x38u;
            store<uint32_t>(rdram, songs + 4u, load<uint32_t>(rdram, songs));
            store<uint32_t>(rdram, headers + 4u, load<uint32_t>(rdram, headers));
            const Live &live = s_shown->live;
            const auto begin = window.songs.begin() + static_cast<std::ptrdiff_t>(window.first);
            if (live.ready)
                live.ready(rdram, ctx, runtime, {begin, begin + static_cast<std::ptrdiff_t>(window.count)});
            window.headers.clear();
            for (size_t r = window.first; r < window.first + window.count; ++r)
            {
                const size_t t = window.tiers[r];
                if (r == window.first || window.tiers[r - 1u] != t)
                {
                    // A live one's by the window's own headers' tokens,
                    // song_header_<name>_<n>, each its text now.
                    std::string token = live.rows ? selected() + "_" + std::to_string(window.headers.size())
                                                  : s_shown->tiers[t].header;
                    if (live.rows)
                        window.headers.push_back(window.tierNames[t]);
                    const uint32_t header[2] = {static_cast<uint32_t>(s_rowTiers.size()),
                                                symbol(rdram, ctx, runtime, token)};
                    pushBack(rdram, ctx, runtime, headers, header, 8u, s_addresses->headersInsertOverflow, scratch);
                }
                const uint32_t data = static_cast<uint32_t>(
                    runtime->callGuestFunction(rdram, ctx, s_addresses->songProviderGetSongData,
                                               {provider, symbol(rdram, ctx, runtime, window.songs[r])}));
                pushBack(rdram, ctx, runtime, songs, &data, 4u, s_addresses->songsInsertOverflow, scratch);
                s_rowTiers.push_back(t);
                s_rowActive.push_back(active[r]);
            }
        }

        // SongProvider::InitData(RndDir *) (0x117448) fills its songs
        // (+0x4c, a vector of each song's config array) from the campaign
        // and its headers (+0x38, {first row, Symbol} each, shown as
        // song_header_<Symbol>). Ours fills both from the setlist, and the
        // last row to +0x48, as it does with no bonus songs.
        void initData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t provider = GPR_U32(ctx, 4);
            s_provider = provider;
            s_shown = ours(rdram, provider);
            s_filled = ps2x::host_clock::now();
            s_rowTiers.clear();
            s_rowActive.clear();
            if (!s_shown)
                return s_initData(rdram, ctx, runtime);
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t scratch = sp - 0x20u;
            SET_GPR_U32(ctx, 29, scratch - 0x10u);
            store<uint32_t>(rdram, provider + 0x5cu, GPR_U32(ctx, 5));
            // Every row it has, then those of its window.
            Window &window = s_windows[selected()];
            window.tiers.clear();
            window.songs.clear();
            std::vector<bool> active;
            if (s_shown->live.rows)
                liveRows(window, s_shown->live, active);
            else
            {
                // The game played's career goes into the save first, as it
                // stands, unsaved or not.
                if (selected() == campaigns::active())
                    save::leaveGame(rdram, ctx, runtime);
                tierRows(rdram, window, *s_shown, active);
            }
            // Not on a song yet this run: on the one it was on last, its
            // window around it.
            if (window.at.empty())
            {
                window.at = save::state("song_at", selected());
                if (const auto at = rowOf(window, window.at))
                    moveTo(window, *at);
            }
            window.first = std::min(window.first, window.songs.empty() ? size_t{0} : window.songs.size() - 1u);
            window.count = windowFrom(window.tiers, window.first);
            pushWindow(rdram, ctx, runtime, provider, scratch, window, active);
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

        // A live setlist's song's venue, `wanted` by GH2's name, or the
        // nearest open one before it (setlists.h's Live). The first two are
        // always open, as retail's quickplay has them
        // (Campaign::IsVenueUnlockedQuickplay, 0x12de88).
        std::string openVenue(const std::string &wanted)
        {
            const std::string &game = campaigns::venueGame();
            const std::vector<std::string> &venues = campaigns::venues(game), &gh2 = campaigns::venues("gh2");
            if (venues.empty())
                return wanted;
            const auto indexIn = [&](const std::vector<std::string> &list)
            { return static_cast<size_t>(std::find(list.begin(), list.end(), wanted) - list.begin()); };
            // Not one of this game's: as far into its venues as into GH2's, else the first.
            size_t at = indexIn(venues);
            if (const size_t inGh2 = indexIn(gh2); at == venues.size())
                at = inGh2 < gh2.size() ? std::min(inGh2, venues.size() - 1u) : 0u;
            while (at > 1u && !campaigns::venueOpen(game, venues[at]))
                --at;
            return venues[at];
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
                // Its tier's venue, or where the game whose venues are
                // played has none of that name (the 80s lack GH2's big and
                // stone), the venue as far into its own list.
                const std::vector<std::string> venues = campaigns::venues(campaigns::venueGame());
                for (const auto &[name, setlist] : s_setlists)
                {
                    // A live one's song is in its window, being played
                    // from it.
                    if (const auto w = s_windows.find(name); setlist.live.rows && w != s_windows.end())
                    {
                        const auto begin = w->second.songs.begin() + static_cast<std::ptrdiff_t>(w->second.first);
                        if (std::find(begin, begin + static_cast<std::ptrdiff_t>(w->second.count), song) !=
                            begin + static_cast<std::ptrdiff_t>(w->second.count))
                            venue = symbol(rdram, ctx, runtime, openVenue(setlist.live.venue(song)));
                    }
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
            }
            SET_GPR_U32(ctx, 2, venue);
            ctx->pc = returnTo;
        }

        // SongProvider::GetHighScore(int row) (0x117b38): the row's score,
        // blank where it has none or in quickplay but for co-op. A career
        // that shows none as a 0 has it so.
        void getHighScore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t provider = GPR_U32(ctx, 4);
            uint32_t score = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->songProviderGetHighScore, {provider, GPR_U32(ctx, 5)}, s_getHighScore));
            const bool career = load<uint32_t>(rdram, provider + 0x30u) == 0u || modeIs(rdram, "multi_coop");
            if (load<uint8_t>(rdram, score) == 0u && career && s_zeroScores.count(campaigns::active()) != 0u)
                score = static_cast<uint32_t>(
                    runtime->callGuestFunction(rdram, ctx, s_addresses->makeStringInt, {s_addresses->scoreFormat, 0u}));
            SET_GPR_U32(ctx, 2, score);
            ctx->pc = returnTo;
        }

        // TextSlot::Fill(const ListProvider &, int, int row, int element)
        // (0x377db8) sets a row's Text to the provider's text for it
        // (SongProvider::Text, 0x1177d8: element 1 the song's name, 6 its
        // blurb, which GH2 has for the last row alone, 0 the tier's header
        // over it). A two-line setlist's row has its header and name, and
        // its Live's line under it, in capitals and cut where it would run
        // under the printout.
        void textSlotFill(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t slot = GPR_U32(ctx, 4), list = GPR_U32(ctx, 5), shown = GPR_U32(ctx, 6);
            const int32_t row = static_cast<int32_t>(GPR_U32(ctx, 7));
            const uint32_t element = GPR_U32(ctx, 8);
            if (!s_shown || !s_shown->twoLines || (element != 0u && element != 1u && element != 6u))
                return s_textSlotFill(rdram, ctx, runtime);
            // The provider as its ListProvider's table puts it.
            const uint32_t table = load<uint32_t>(rdram, list);
            const uint32_t provider = list + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, table + 0x10u)));
            const Window &window = s_windows[selected()];
            const size_t at = window.first + static_cast<size_t>(std::max(row, 0));
            if (provider != s_provider || at >= window.songs.size())
                return s_textSlotFill(rdram, ctx, runtime);
            const uint32_t returnTo = GPR_U32(ctx, 31), sp = GPR_U32(ctx, 29);
            std::string line;
            if (element != 6u)
            {
                const uint32_t name = static_cast<uint32_t>(runtime->callGuestFunction(
                    rdram, ctx, s_addresses->songProviderText, {provider, shown, static_cast<uint32_t>(row), element}));
                line = name ? reinterpret_cast<const char *>(getMemPtr(rdram, name)) : "";
            }
            else
                line = s_shown->live.under(window.songs[at]);
            const uint32_t text = load<uint32_t>(rdram, slot + 4u);
            line = setlist_look::rowText(rdram, ctx, runtime, text, line, element);
            // On the guest's stack, below what it holds.
            constexpr size_t kMost = 0x1f0u;
            const uint32_t buffer = (sp - 0x200u) & ~0xfu;
            line.resize(std::min(line.size(), kMost - 1u));
            std::memcpy(getMemPtr(rdram, buffer), line.c_str(), line.size() + 1u);
            SET_GPR_U32(ctx, 29, buffer - 0x10u);
            runtime->callGuestFunction(rdram, ctx, s_addresses->rndTextSetText, {text, buffer});
            SET_GPR_U32(ctx, 29, sp);
            ctx->pc = returnTo;
        }

        // The shown one's song list scene. The game played's own list is in
        // its own scene, at the game's path for it.
        std::string shownLook(const std::string &name = selected())
        {
            return name == campaigns::active() ? kOwnLook : s_setlists.at(name).look;
        }

        script::Node setlistCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "select" || op == "next" || op == "take")
            {
                const std::string name = op == "take" ? s_next : call.symbol(2);
                const bool changed = s_setlists.count(name) != 0u && name != selected();
                s_next = op == "next" && changed ? name : "";
                if (changed && op != "next")
                {
                    s_selected = name;
                    save::keepState("setlist", "shown", s_selected);
                    save::keepState("setlist", "game", s_campaign);
                }
                return {changed ? 1u : 0u, script::kInt};
            }
            // The next one's, for the list it turns to, loaded as it is picked.
            if (op == "look")
                return {symbol(call.rdram, call.ctx, call.runtime, shownLook(s_next.empty() ? selected() : s_next)),
                        script::kSymbol};
            if (op == "shown")
                return {symbol(call.rdram, call.ctx, call.runtime, selected()), script::kSymbol};
            if (op == "has")
                return {has(call.symbol(2)) ? 1u : 0u, script::kInt};
            // A live one's rows changed since the list filled: at most once
            // a second, as filling takes most of a frame.
            if (op == "stale")
            {
                const bool stale = s_shown && s_shown->live.rows &&
                                   ps2x::host_clock::now() - s_filled >=std::chrono::seconds(1) &&
                                   s_shown->live.rows() != s_windows[selected()].from;
                return {stale ? 1u : 0u, script::kInt};
            }
            // The text of the shown one's window's header (a live one's).
            if (op == "header")
            {
                const Window &w = s_windows[selected()];
                const size_t n = call.index(2);
                return {symbol(call.rdram, call.ctx, call.runtime, n < w.headers.size() ? w.headers[n] : ""),
                        script::kSymbol};
            }
            Window &window = s_windows[selected()];
            if (const auto paper = setlist_paper::command(call, shownLook(), window.tiers, window.first))
                return *paper;
            // A row, -1 for none.
            const auto rowNode = [](std::optional<size_t> row) -> script::Node
            { return {row ? static_cast<uint32_t>(*row) : static_cast<uint32_t>(-1), script::kInt}; };
            // The shown one's song last on, its row in the list, -1 if none
            // yet; with a row, that one's song. Kept by song, the list's
            // rows being the window's.
            if (op == "song")
            {
                // The game's own quickplay list, which it fills itself, by
                // row ([row_at]); a career's is on its own song.
                if (selected() == campaigns::active())
                {
                    if (!modeIs(call.rdram, "quickplay") || s_provider == 0u)
                        return rowNode(std::nullopt);
                    if (call.size() > 2)
                        save::keepState("row_at", selected(), std::to_string(static_cast<int>(call.number(2))));
                    const std::string kept = save::state("row_at", selected());
                    const int last = load<int32_t>(call.rdram, s_provider + 0x48u);
                    const int row = kept.empty() ? -1 : std::clamp(std::atoi(kept.c_str()), 0, std::max(0, last));
                    return {static_cast<uint32_t>(row), script::kInt};
                }
                // Only on its own list: a career's or co-op's, on the same
                // panel, neither reads nor moves it.
                if (s_provider == 0u || !ours(call.rdram, s_provider))
                    return rowNode(std::nullopt);
                if (call.size() > 2)
                {
                    const size_t r = window.first + call.index(2);
                    if (r < window.songs.size())
                    {
                        window.at = window.songs[r];
                        save::keepState("song_at", selected(), window.at);
                        window.atRow = r - window.first;
                    }
                }
                const auto found = rowOf(window, save::state("song_at", selected()));
                if (!found)
                    return rowNode(std::nullopt);
                const int row = static_cast<int>(*found) - static_cast<int>(window.first);
                return {static_cast<uint32_t>(std::clamp(row, 0, static_cast<int>(window.count) - 1)), script::kInt};
            }
            // A live one's tiers: how many, one's header text, the one the
            // list is on, and the list moved to one's first song (its row
            // in the window, the provider to be filled again).
            if (op == "tiers")
                return {static_cast<uint32_t>(window.tierNames.size()), script::kInt};
            if (op == "tier_name")
            {
                const size_t n = call.index(2);
                return {symbol(call.rdram, call.ctx, call.runtime, n < window.tierNames.size() ? window.tierNames[n] : ""),
                        script::kSymbol};
            }
            if (op == "tier_at")
            {
                const auto at = rowOf(window, window.at);
                return {static_cast<uint32_t>(at ? window.tiers[*at] : 0u), script::kInt};
            }
            if (op == "jump" || op == "random")
            {
                const auto row = op == "jump" ? jump(window, call.index(2)) : random(window);
                if (row)
                    save::keepState("song_at", selected(), window.at);
                return rowNode(row);
            }
            // The list's row now on: the row it is on once the window has
            // moved to keep it away from either end, -1 if it has not.
            if (op == "scroll")
                return rowNode(s_shown ? scroll(window, call.index(2)) : std::nullopt);
            std::cerr << "[setlist] usage: {setlist select|next|has <name>}, {setlist take|look|shown|tiers|tier_at|random}, "
                      << "{setlist song [row]}, {setlist scroll|frame <row>}, {setlist header|tier_name|jump <n>}, "
                      << "{setlist paper_base <piece> <x> <z>}, {setlist paper <piece> x|z}, "
                      << "{setlist paper_pose <piece> <object>} (content/setlists.h)" << std::endl;
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

    void addLive(const std::string &name, const std::string &look, Live live)
    {
        for (size_t n = 0; n < kLiveHeaders; ++n)
            locale::addExpression("song_header_" + name + "_" + std::to_string(n),
                                  "{setlist header " + std::to_string(n) + "}");
        const auto twoLines = live.under ? setlist_look::addTwoLineLook(name, look) : std::nullopt;
        s_setlists[name] = {{}, twoLines.value_or(look), {}, std::move(live), twoLines.has_value()};
    }

    void addLook(const std::string &game, milo::Dir scene)
    {
        setlist_look::addScene(game + "/" + setlist_look::kOwnScene, std::move(scene));
    }

    Required required(const dtb::Node &campaign)
    {
        Required out;
        if (const dtb::Node *counts = dtb::find(campaign, "required_songs"))
            for (size_t d = 1u; d < counts->nodes.size(); ++d)
                if (counts->nodes[d].nodes.size() > 1u)
                    out.push_back({counts->nodes[d].nodes[1].integer, counts->nodes[d].nodes.back().integer});
        return out;
    }

    void zeroScores(const std::string &game)
    {
        s_zeroScores.insert(game);
    }

    namespace
    {
        std::vector<std::string> tierSongs(const std::string &name, bool career)
        {
            std::vector<std::string> songs;
            if (const auto it = s_setlists.find(name); it != s_setlists.end())
                for (const Tier &tier : it->second.tiers)
                    if (tier.encore == career)
                        songs.insert(songs.end(), tier.songs.begin(), tier.songs.end());
            return songs;
        }
    }

    std::vector<std::string> careerSongs(const std::string &name) { return tierSongs(name, true); }

    std::optional<double> careerAt(const std::string &name, const std::string &song)
    {
        const auto it = s_setlists.find(name);
        if (it == s_setlists.end())
            return std::nullopt;
        std::vector<const Tier *> career;
        for (const Tier &tier : it->second.tiers)
            if (tier.encore) // marks a career tier
                career.push_back(&tier);
        for (size_t t = 0; t < career.size(); ++t)
            if (std::find(career[t]->songs.begin(), career[t]->songs.end(), song) != career[t]->songs.end())
                return career.size() > 1u ? static_cast<double>(t) / static_cast<double>(career.size() - 1u) : 0.0;
        return std::nullopt;
    }

    std::vector<std::string> bonusSongs(const std::string &name) { return tierSongs(name, false); }

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
        const auto file = ark::readFile(disc, setlist_look::kOwnScene);
        if (const auto scene = file ? milo::read(*file) : std::nullopt)
            addLook(game, *scene);
        std::array<std::string, 5> scoreNames;
        for (size_t i = 0; i < scoreNames.size(); ++i)
            scoreNames[i] = text("highscore_dummy_" + std::to_string(i));
        add(game, std::move(tiers), game + "/" + kOwnLook, scoreNames, required(*campaign));
    }

    const std::string &shown() { return selected(); }

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
        s_getHighScore = runtime.lookupFunction(addresses.songProviderGetHighScore);
        runtime.replaceFunction(addresses.songProviderGetHighScore, &getHighScore);
        s_textSlotFill = runtime.lookupFunction(addresses.textSlotFill);
        runtime.replaceFunction(addresses.textSlotFill, &textSlotFill);
    }
}
