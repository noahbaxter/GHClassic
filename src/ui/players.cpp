#include "ui/players.h"

#include "content/locale.h"
#include "dta.h"
#include "host/input.h"
#include "save/profiles.h"
#include "script.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace gh2
{
    namespace
    {
        // What the panel last drew from.
        std::string s_drawn[kPlayers];
        bool s_step = false; // Multiplayer's screen is up

        // Each seat's controller as `joined` last saw it.
        std::string s_seen[kPlayers];
        uint32_t s_seenStarts = 0; // startSeats() then

        void see()
        {
            for (int seat = 0; seat < kPlayers; ++seat)
                s_seen[seat] = seatDevice(seat);
        }

        std::string lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        // Who holds a seat: the profile it plays as, else GUEST; "" for no
        // controller.
        std::string holder(int seat)
        {
            if (seatDevice(seat).empty())
                return "";
            const auto profile = save::seatProfile(seat);
            return profile ? save::profiles().names[static_cast<size_t>(*profile)] : "GUEST";
        }

        // dta/profiles.dta's, a seat by number from 0, a profile by its row
        // (save/profiles.h, from 0):
        // {ghc_profile named <row>}: whether it is a profile's.
        // {ghc_profile name_of <row>}: that profile's name.
        // {ghc_profile name <seat>}: the seat's profile's name, "" for a guest.
        // {ghc_profile current <seat>}: the seat's profile, -1 for a guest.
        // {ghc_profile badge <seat>}: "P1  Noah", "P1  GUEST", "" for none.
        // {ghc_profile pick <seat> <row>}: it plays as that one, -1 a guest.
        // {ghc_profile taken <name>}: whether a profile has it, any case.
        // {ghc_profile make <seat> <name>}: a new profile it plays as, in the
        //   first free row, so the profiles stay at the top.
        // {ghc_profile hold <0|1>}: a menu is up, so no controller takes,
        //   leaves or trades a seat (host/input.h).
        // {ghc_profile drop <seat>}: its player drops out.
        // {ghc_profile joined <menu>}: a seat taken by another controller
        //   since the last call, on Multiplayer's screen or, with <menu> 1,
        //   the main menu's, where whoever presses takes P1; -1 for none.
        // {ghc_profile seen}: who sits now is no join, as a menu opens.
        script::Node profile(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            const auto word = [&](const std::string &text) { return call.word(text); };
            const auto number = [](int n) { return script::Node{static_cast<uint32_t>(n), script::kInt}; };
            const int n = call.size() > 2 && op != "taken" ? static_cast<int>(call.number(2)) : 0;
            const save::Profiles profiles = save::profiles();
            if (op == "named")
                return number(profiles.here(n) ? 1 : 0);
            if (op == "name_of")
                return word(profiles.here(n) ? profiles.names[static_cast<size_t>(n)] : "");
            if (op == "name")
            {
                const auto mine = save::seatProfile(n);
                return word(mine ? profiles.names[static_cast<size_t>(*mine)] : "");
            }
            if (op == "current")
                return number(save::seatProfile(n).value_or(-1));
            if (op == "badge")
            {
                const std::string who = holder(n);
                return word(who.empty() ? who : seatTag(n) + who);
            }
            if (op == "pick")
            {
                const int row = static_cast<int>(call.number(3));
                save::keepSeatProfile(n, row >= 0 ? std::optional<int>(row) : std::nullopt);
                return {};
            }
            if (op == "taken")
            {
                const std::string name = lower(call.text(2));
                return number(std::any_of(profiles.names.begin(), profiles.names.end(),
                                          [&](const std::string &p) { return !p.empty() && lower(p) == name; }));
            }
            if (op == "make")
            {
                if (const auto row = profiles.free())
                {
                    save::nameProfile(*row, call.text(3));
                    save::keepSeatProfile(n, row);
                }
                return {};
            }
            if (op == "hold")
            {
                holdSeats(n != 0);
                return {};
            }
            if (op == "drop")
            {
                dropSeat(n);
                return {};
            }
            if (op == "seen")
            {
                see();
                s_seenStarts = startSeats();
                return {};
            }
            if (op == "joined")
            {
                // One seated nowhere before: two trading places is no join.
                // On the main menu by its Start alone, which the game never
                // saw: a fret or strum taking P1 is the one player
                // switching controllers.
                const uint32_t starts = startSeats();
                const bool any = s_step || (n != 0 && starts != s_seenStarts);
                int joined = -1;
                std::string now[kPlayers];
                for (int seat = 0; seat < kPlayers; ++seat)
                    now[seat] = seatDevice(seat);
                for (int seat = 0; seat < kPlayers && joined < 0; ++seat)
                    if (any && !now[seat].empty() &&
                        std::find(std::begin(s_seen), std::end(s_seen), now[seat]) == std::end(s_seen))
                        joined = seat;
                std::copy(std::begin(now), std::end(now), std::begin(s_seen));
                s_seenStarts = starts;
                return number(joined);
            }
            return {};
        }

        // {ghc_players step <0|1>}: Multiplayer's screen came up or went.
        // {ghc_players changed}: 1 once each time a seat's player changes.
        // {ghc_players row <seat>}: that seat's line. {ghc_players trades}:
        // how many times P1 and P2 have traded places.
        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "step")
            {
                const bool on = call.number(2) != 0.0f;
                s_step = on;
                // Who sits as it comes up joined before it.
                see();
                setPlayersStep(on);
                return {};
            }
            if (op == "changed")
            {
                bool changed = false;
                for (int seat = 0; seat < kPlayers; ++seat)
                {
                    const std::string label = holder(seat);
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
        script::addCommand("ghc_profile", profile);
        // Profiles' (dta/profiles.dta, patched with the menus, which hold it).
        locale::add("ghc_choose_profile", "CHOOSE PROFILE");
        locale::add("ghc_profile_p0", "P1 PROFILE");
        locale::add("ghc_profile_p1", "P2 PROFILE");
        locale::add("ghc_name_profile", "NAME YOUR PROFILE");
        locale::add("ghc_new_profile", "new profile");
        locale::add("ghc_guest", "guest");
        locale::add("ghc_profile_change", "CHANGE PROFILE");
        locale::add("ghc_profile_drop", "DROP OUT");
        script::patchUi(kPlayersDta);
        // Multiplayer's help bar (players.dta).
        locale::add("ghc_players_reorder", "REORDER");
    }
}
