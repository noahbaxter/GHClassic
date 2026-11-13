#include "save/results.h"
#include "content/song_speed.h"

#include "gameplay/engine/note_engine.h"
#include "hook.h"
#include "host/input.h"
#include "save/profiles.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <iostream>

namespace gh2::save
{
    namespace
    {
        std::function<std::optional<NamedSong>(const std::string &)> s_namer;

        uint32_t count(const script::Call &call, int i)
        {
            return static_cast<uint32_t>(std::max(0.0f, call.number(i)));
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
        script::addCommand("best", bestCommand);
        EntryHook<GameOverTag>::install(runtime, addresses.gamePanelSetGameOver, onGameOver);
    }
}
