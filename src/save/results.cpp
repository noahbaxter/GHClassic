#include "save/results.h"

#include "gameplay/engine/note_engine.h"
#include "hook.h"
#include "save/gh2_text.h"
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

        // {best finished <mode> <song> <difficulty> <part> <score> <stars>
        //   <percent> <streak> <hit> <passed>}   a run won, from onGameOver;
        //   kept where it was alone, in quickplay or a career
        // {best score <song> <difficulty>}       its best score as played
        //   now, 0 for none
        script::Node bestCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "finished")
            {
                const std::string mode = call.symbol(2);
                if (mode != "quickplay" && mode != "career")
                    return {};
                Best run{count(call, 6), count(call, 7), count(call, 8), count(call, 9)};
                const uint32_t hit = count(call, 10), passed = count(call, 11);
                // As the campaign hub's full combos: every gem hit in one streak.
                run.fullCombo = hit > 0u && passed == 0u && run.streak == hit;
                keepBest(playedKey(call.symbol(3), call.symbol(4), call.symbol(5)), run);
                return {};
            }
            if (op == "score")
            {
                const auto best = keptBest(playedKey(call.symbol(2), call.symbol(3)));
                return {best ? best->score : 0u, script::kInt};
            }
            std::cerr << "[best] usage: {best score <song> <difficulty>}" << std::endl;
            return {};
        }

        struct GameOverTag;

        // GamePanel::SetGameOver(won), before any screen of the song's end.
        void onGameOver(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 5) == 0u)
                return;
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime,
                        "{if {== {game num_players} 1} "
                        "{best finished {gamecfg get mode} {game get_song} {game get_difficulty_sym 0} "
                        "{gamecfg get_track_sym 0} {player0 score} {player0 num_stars} {player0 percent_hit} "
                        "{player0 longest_streak} {player0 gems_hit} {player0 gems_passed}}}");
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
        const std::string game = name ? "" : gameOfSong(song);
        return bestKey(name ? name->song : game + '.' + gh2::storedSong(game, song), noteEngineName(noteEngine()),
                       name ? name->part : part, difficulty, controller);
    }

    void installResults(PS2Runtime &runtime, const Addresses &addresses)
    {
        script::addCommand("best", bestCommand);
        EntryHook<GameOverTag>::install(runtime, addresses.gamePanelSetGameOver, onGameOver);
    }
}
