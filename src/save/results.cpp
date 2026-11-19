#include "save/results.h"
#include "content/song_speed.h"

#include "gameplay/engine/note_engine.h"
#include "guest.h"
#include "hook.h"
#include "host/input.h"
#include "host/song_log.h"
#include "save/profiles.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace gh2::save
{
    namespace
    {
        std::function<std::optional<NamedSong>(const std::string &)> s_namer;
        const Addresses *s_addresses = nullptr;

        uint32_t count(const script::Call &call, int i)
        {
            return static_cast<uint32_t>(std::max(0.0f, call.number(i)));
        }

        // A named song's high score table, as GH2 keeps a disc song's
        // (HighScoreList, 0x13c650: five, best first) but in the save:
        //
        //   [scores custom.3f9a12c4d5e6f708.guitar]
        //   expert = 37000 0 highscore_dummy_0|29000 0 highscore_dummy_1|...
        //
        // each entry its score, 1 for a player's or 0 for a seed, and name,
        // a seed's the token of GH2's (SongHighScores, 0x13cc20).
        struct Entry
        {
            uint32_t score = 0u;
            bool real = false;
            std::string name;
        };
        constexpr size_t kEntries = 5u;

        std::string tableSection(const NamedSong &song)
        {
            return "scores " + song.song + '.' + song.part;
        }

        std::vector<Entry> table(const std::string &section, const std::string &difficulty)
        {
            std::vector<Entry> out;
            const Section *kept = keptSection(section);
            const std::string *text = kept ? kept->get(difficulty) : nullptr;
            for (size_t at = 0u; text && at < text->size();)
            {
                const size_t end = std::min(text->find('|', at), text->size());
                const std::string item = text->substr(at, end - at);
                const size_t a = item.find(' '), b = a == std::string::npos ? a : item.find(' ', a + 1u);
                if (b != std::string::npos)
                    out.push_back({static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 10)), item[a + 1u] == '1',
                                   item.substr(b + 1u)});
                at = end + 1u;
            }
            return out;
        }

        void keepTable(const std::string &section, const std::string &difficulty, const std::vector<Entry> &entries)
        {
            std::string text;
            for (const Entry &e : entries)
                text += (text.empty() ? "" : "|") + std::to_string(e.score) + (e.real ? " 1 " : " 0 ") + e.name;
            keepSection(section, [&](Section &s) { s.set(difficulty, text); });
        }

        // GH2 seeds every table 50000 down to 10000 whatever the song; a
        // named song's sit between its chart's 3 and 4 stars, so any 4 stars
        // beats them all: from 1.9 its base score down to 1.1, to the hundred
        // below. Four stars is twice the base (scoring.dta's star_ratings,
        // 2.0 on every difficulty; Scoring::GetScoreForStars, 0x11c670).
        std::vector<Entry> seeded(uint32_t base)
        {
            std::vector<Entry> out;
            for (size_t i = 0u; i < kEntries; ++i)
            {
                const uint32_t score = base * static_cast<uint32_t>(19u - 2u * i) / 10u / 100u * 100u;
                out.push_back({std::max(score, 100u), false, "highscore_dummy_" + std::to_string(i)});
            }
            return out;
        }

        // A player's chart's base score, its stars' measure (GetStarsForScore,
        // 0x11c728).
        uint32_t baseScore(const script::Call &call, int player)
        {
            const uint32_t songDb = load<uint32_t>(call.rdram, s_addresses->theSongDb);
            if (songDb == 0u)
                return 0u;
            return static_cast<uint32_t>(call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->songDbGetBaseMaxPoints,
                                                                         {songDb, static_cast<uint32_t>(player)}));
        }

        // {ghc_scores place <song> <difficulty> <score> <seat>}   where the
        //   score goes on its table, from 1, 0 for nowhere; a table seeded
        //   for the chart that seat played where there is none
        // {ghc_scores name|score|seed <song> <difficulty> <n>}   its nth
        //   entry's from 0: name (a seed's a locale token), score, whether
        //   it is a seed
        // {ghc_scores add <song> <difficulty> <name> <score>}   a player's
        //   score on it, in its place
        script::Node scoresCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            const auto song = s_namer ? s_namer(call.text(2)) : std::nullopt;
            if (!song)
                return {};
            const std::string section = tableSection(*song), difficulty = call.symbol(3);
            std::vector<Entry> entries = table(section, difficulty);
            const auto place = [&](uint32_t score)
            {
                const auto at = std::find_if(entries.begin(), entries.end(), [&](const Entry &e) { return score > e.score; });
                return static_cast<size_t>(at - entries.begin());
            };
            if (op == "place")
            {
                if (entries.empty())
                {
                    entries = seeded(baseScore(call, static_cast<int>(call.number(5))));
                    keepTable(section, difficulty, entries);
                }
                const size_t at = place(count(call, 4));
                return {at < kEntries ? static_cast<uint32_t>(at + 1u) : 0u, script::kInt};
            }
            if (op == "add")
            {
                const uint32_t score = count(call, 5);
                entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(place(score)), {score, true, call.text(4)});
                entries.resize(std::min(entries.size(), kEntries));
                keepTable(section, difficulty, entries);
                return {};
            }
            const size_t n = static_cast<size_t>(std::max(0.0f, call.number(4)));
            const Entry e = n < entries.size() ? entries[n] : Entry{};
            if (op == "name")
                return call.word(e.name);
            if (op == "score")
                return {e.score, script::kInt};
            if (op == "seed")
                return {e.real || e.name.empty() ? 0u : 1u, script::kInt};
            std::cerr << "[ghc_scores] usage: {ghc_scores place|name|score|seed|add <song> <difficulty> ...}" << std::endl;
            return {};
        }

        // The bests section a mode's runs go in (bestSection): solo's for
        // quickplay and a career, none for the rest (practice, tutorials).
        std::optional<std::string> modeBests(const std::string &mode)
        {
            if (mode == "quickplay" || mode == "career")
                return "";
            if (mode == "multi_coop")
                return "coop";
            if (mode == "multi_vs")
                return "faceoff";
            if (mode == "multi_fo")
                return "pro_faceoff";
            return std::nullopt;
        }

        // {best finished <mode> <seat> <song> <difficulty> <part> <score>
        //   <stars> <percent> <streak> <hit> <passed>}   a seat's run of a
        //   song won, from onGameOver; kept by mode, each player's own
        //   score in multiplayer as GH2's endgame shows it, as the profile
        //   the seat plays as
        // {best score <song> <difficulty> [<seat> <mode>]}   its best score
        //   as the seat (P1's by default) plays it now, in that mode (as
        //   gamecfg's, solo's by default), 0 for none
        script::Node bestCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "finished")
            {
                const auto mode = modeBests(call.symbol(2));
                if (!mode)
                    return {};
                const int seat = static_cast<int>(call.number(3));
                Best run{count(call, 7), count(call, 8), count(call, 9), count(call, 10)};
                const uint32_t hit = count(call, 11), passed = count(call, 12);
                // As the campaign hub's full combos: every gem hit in one streak.
                run.fullCombo = hit > 0u && passed == 0u && run.streak == hit;
                keepBest(playedKey(call.symbol(4), call.symbol(5), call.symbol(6), seatController(seat)),
                         seatProfile(seat), run, *mode);
                return {};
            }
            if (op == "score")
            {
                const int seat = call.size() > 4 ? static_cast<int>(call.number(4)) : 0;
                const auto mode = call.size() > 5 ? modeBests(call.symbol(5)) : std::optional<std::string>("");
                if (!mode)
                    return {0u, script::kInt};
                const auto best = keptBest(playedKey(call.symbol(2), call.symbol(3), "guitar", seatController(seat)),
                                           seatProfile(seat), *mode);
                return {best ? best->score : 0u, script::kInt};
            }
            std::cerr << "[best] usage: {best score <song> <difficulty> [<seat> <mode>]}" << std::endl;
            return {};
        }

        struct GameOverTag;

        // GamePanel::SetGameOver(won), before any screen of the song's end.
        void onGameOver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            song_log::gameOver(GPR_U32(ctx, 5) != 0u);
            if (GPR_U32(ctx, 5) == 0u)
                return;
            const R5900Context saved = *ctx;
            for (const char *seat : {"0", "1"})
            {
                const std::string n = seat, player = std::string("{player") + seat;
                script::run(rdram, ctx, runtime,
                            "{if {> {game num_players} " + n + "} {best finished {gamecfg get mode} " + n +
                                " {game get_song} {game get_difficulty_sym " + n + "} {gamecfg get_track_sym " + n +
                                "} " + player + " score} " + player + " num_stars} " + player + " percent_hit} " +
                                player + " longest_streak} " + player + " gems_hit} " + player + " gems_passed}}}");
            }
            *ctx = saved;
        }
    }

    void nameSongs(std::function<std::optional<NamedSong>(const std::string &song)> namer)
    {
        s_namer = std::move(namer);
    }

    bool isNamed(const std::string &song)
    {
        return s_namer && s_namer(song);
    }

    std::string playedKey(const std::string &song, const std::string &difficulty, const std::string &part,
                          const std::string &controller)
    {
        const auto name = s_namer ? s_namer(song) : std::nullopt;
        const int speed = song_speed::percent();
        return bestKey(name ? name->song : gameOfSong(song) + '.' + song, noteEngineName(noteEngine()),
                       name ? name->part : part, difficulty, controller) +
               (speed == 100 ? "" : ".s" + std::to_string(speed));
    }

    std::string seatController(int seat)
    {
        return seatPad(seat) ? "gamepad" : "guitar";
    }

    void installResults(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("best", bestCommand);
        script::addCommand("ghc_scores", scoresCommand);
        EntryHook<GameOverTag>::install(runtime, addresses.gamePanelSetGameOver, onGameOver);
    }
}
