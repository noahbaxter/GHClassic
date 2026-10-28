#include "ui/players.h"

#include "content/locale.h"
#include "dta.h"
#include "host/input.h"
#include "script.h"

#include <string>

namespace gh2
{
    namespace
    {
        // What the panel last drew from.
        std::string s_drawn[kPlayers];

        // {ghc_players step <0|1>}: Multiplayer's screen came up or went.
        // {ghc_players changed}: 1 once each time a seat's player changes.
        // {ghc_players row <seat>}: that seat's line. {ghc_players trades}:
        // how many times P1 and P2 have traded places.
        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "step")
            {
                setPlayersStep(call.number(2) != 0.0f);
                return {};
            }
            if (op == "changed")
            {
                bool changed = false;
                for (int seat = 0; seat < kPlayers; ++seat)
                {
                    const std::string label = seatLabel(seat);
                    changed = changed || label != s_drawn[seat];
                    s_drawn[seat] = label;
                }
                return {changed ? 1u : 0u, script::kInt};
            }
            if (op == "trades")
                return {seatTrades(), script::kInt};
            const int seat = static_cast<int>(call.number(2));
            if (op == "row" && seat >= 0 && seat < kPlayers)
            {
                const std::string text =
                    seatTag(seat) + (s_drawn[seat].empty() ? std::string("press START to join") : s_drawn[seat]);
                return {script::symbol(call.rdram, call.ctx, call.runtime, text), script::kSymbol};
            }
            return {};
        }
    }

    std::string seatTag(int seat)
    {
        return "P" + std::to_string(seat + 1) + "  ";
    }

    void installPlayers(PS2Runtime &)
    {
        script::addCommand("ghc_players", command);
        script::patchUi(kPlayersDta);
        // Multiplayer's help bar (players.dta).
        locale::add("ghc_players_reorder", "REORDER");
    }
}
