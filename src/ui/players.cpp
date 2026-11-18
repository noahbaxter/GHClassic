#include "ui/players.h"

#include "content/games.h"
#include "content/locale.h"
#include "disc/ark.h"
#include "dta.h"
#include "highway/note_speed.h"
#include "host/input.h"
#include "render/texture_store.h"
#include "save/profiles.h"
#include "save/save.h"
#include "script.h"
#include "ui/seat_colors.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

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

        // Each seat's profile and lefty as `lefty` last saw them, once it
        // has. A guest's lefty is kept apart, in state.ini's [guest], as
        // Options' holds whoever played last.
        std::optional<int> s_leftyFor[kPlayers];
        bool s_leftyWas[kPlayers] = {};
        bool s_leftySeen[kPlayers] = {};

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

        // The poster's red in `to`, its cream, black and paper left as they
        // are: each pixel by how red it is, by hue and saturation, takes the
        // seat's hue and its saturation and brightness as far from the
        // poster's red as its own are.
        void recolor(std::vector<uint8_t> &rgba, const std::array<float, 3> &to)
        {
            const auto hsv = [](float r, float g, float b, float &h, float &s, float &v)
            {
                v = std::max({r, g, b});
                const float c = v - std::min({r, g, b});
                s = v > 0.0f ? c / v : 0.0f;
                h = c == 0.0f   ? 0.0f
                    : v == r    ? std::fmod((g - b) / c + 6.0f, 6.0f)
                    : v == g    ? (b - r) / c + 2.0f
                                : (r - g) / c + 4.0f;
                h *= 60.0f;
            };
            // The poster's red's own saturation and brightness.
            constexpr float kRedS = 0.82f, kRedV = 0.78f;
            float th = 0.0f, ts = 0.0f, tv = 0.0f;
            hsv(to[0], to[1], to[2], th, ts, tv);
            for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                const float r = rgba[i] / 255.0f, g = rgba[i + 1] / 255.0f, b = rgba[i + 2] / 255.0f;
                float h = 0.0f, s = 0.0f, v = 0.0f;
                hsv(r, g, b, h, s, v);
                const float off = std::min(h, 360.0f - h); // degrees from red
                const float red = std::clamp((40.0f - off) / 20.0f, 0.0f, 1.0f) * std::clamp((s - 0.3f) / 0.2f, 0.0f, 1.0f);
                if (red <= 0.0f)
                    continue;
                const float ns = std::min(1.0f, s * ts / kRedS), nv = std::min(1.0f, v * tv / kRedV);
                // hsv back to rgb at the seat's hue.
                const float c = nv * ns, x = c * (1.0f - std::fabs(std::fmod(th / 60.0f, 2.0f) - 1.0f)), m = nv - c;
                const int sector = static_cast<int>(th / 60.0f) % 6;
                const std::array<float, 3> out = sector == 0   ? std::array<float, 3>{c, x, 0}
                                                 : sector == 1 ? std::array<float, 3>{x, c, 0}
                                                 : sector == 2 ? std::array<float, 3>{0, c, x}
                                                 : sector == 3 ? std::array<float, 3>{0, x, c}
                                                 : sector == 4 ? std::array<float, 3>{x, 0, c}
                                                               : std::array<float, 3>{c, 0, x};
                const float was[3] = {r, g, b};
                for (int k = 0; k < 3; ++k)
                    rgba[i + size_t(k)] =
                        static_cast<uint8_t>(std::lround(255.0f * (was[k] + (out[size_t(k)] + m - was[k]) * red)));
            }
        }

        // GH2's popup poster in each seat's colour from each game's menus
        // (ui/seat_colors.h), a picture of its own for each, its bitmap
        // ui/image/gen/ghc_poster_<game>_<seat>.png_ps2 naming it: a player's
        // Start menu (dta/profiles.dta). Drawn from the 360's poster where
        // that disc is, else the PS2's.
        void seatPosters()
        {
            textures::Source base{0u, "ui/gen/dialog.milo_ps2", "dl_poster.tex", true};
            if (const auto xbox = games::disc("gh2x"))
                base = {*xbox, "ui/gen/dialog.milo_xbox", "dl_poster.tex"};
            for (const seat_colors::Game &game : seat_colors::kGames)
                for (size_t seat = 0; seat < game.seats.size(); ++seat)
                {
                    const std::string name = std::string("ghc_poster_") + game.game + "_" + std::to_string(seat);
                    textures::Source source = base;
                    source.variant = name;
                    const uint32_t id = textures::add(source);
                    textures::addEdit(id,
                                      [to = game.seats[seat]](TextureData &data)
                                      {
                                          recolor(data.rgba, to);
                                          for (std::vector<uint8_t> &mip : data.mips)
                                              recolor(mip, to);
                                      });
                    ark::addFile("ui/image/gen/" + name + ".png_ps2", textures::standIn(id, 256u, 512u));
                }
            // A strip of its black, the name's bar: its paper's grain and
            // creases, so the bar is printed with the rest.
            textures::Source black = base;
            black.variant = "ghc_poster_black";
            const uint32_t id = textures::add(black);
            textures::addEdit(id,
                              [](TextureData &data)
                              {
                                  const uint32_t x0 = data.width / 5u, x1 = data.width * 4u / 5u;
                                  const uint32_t y0 = data.height * 55u / 100u, y1 = data.height * 65u / 100u;
                                  std::vector<uint8_t> strip;
                                  for (uint32_t y = y0; y < y1; ++y)
                                      strip.insert(strip.end(), data.rgba.begin() + (y * data.width + x0) * 4u,
                                                   data.rgba.begin() + (y * data.width + x1) * 4u);
                                  data.rgba = std::move(strip);
                                  data.width = x1 - x0;
                                  data.height = y1 - y0;
                                  data.mips.clear();
                              });
            ark::addFile("ui/image/gen/ghc_poster_black.png_ps2", textures::standIn(id, 128u, 32u));
        }

        // Who holds a seat: the profile it plays as, else GUEST; "" for one
        // not signed in.
        std::string holder(int seat)
        {
            if (!save::seatSignedIn(seat))
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
        // {ghc_profile holder <seat>}: "Noah", "GUEST", "" for none signed
        //   in.
        // {ghc_profile badge <seat>}: "P1  Noah", "P1  GUEST", "" for none.
        // {ghc_profile ink <seat> <game> <colour>}: a packed red (pack_color)
        //   in the seat's colour from that game's menus, as its poster's.
        // {ghc_profile pick <seat> <row>}: it plays as that one, -1 a guest.
        // {ghc_profile taken <name> [<row>]}: whether a profile has it, any
        //   case, but for the one in <row>.
        // {ghc_profile seat_of <row>}: the seat that plays as it, -1 for none.
        // {ghc_profile rename <row> <name>}, {ghc_profile delete <row>}.
        // {ghc_profile make <seat> <name>}: a new profile it plays as, in the
        //   first free row, so the profiles stay at the top.
        // {ghc_profile hold <0|1>}: a menu is up, so no controller takes,
        //   leaves or trades a seat (host/input.h).
        // {ghc_profile joins <0|1>}: the main menu is up, where Start joins.
        // {ghc_profile drop <seat>}: its player signs out and drops out.
        // {ghc_profile joined <menu>}: a seat taken by another controller
        //   since the last call, on Multiplayer's screen or, with <menu> 1,
        //   the main menu's, where whoever presses takes P1; -1 for none.
        // {ghc_profile seen}: who sits now is no join, as a menu opens.
        // {ghc_profile pref <seat> speed|length}: what it plays with, its
        //   profile's own else the game's (highway/note_speed.h).
        // {ghc_profile set_pref <seat> lefty|speed|length <value>}: on its
        //   profile; a guest's goes nowhere.
        // {ghc_profile shown <value>}: a speed or length as Game Settings
        //   shows it, in tenths: 60 "6.0".
        // {ghc_profile lefty <seat> <now>}: whether the seat plays left
        //   handed, now <now>: a profile sitting down brings its own, and one
        //   sat there keeps whatever it is changed to; a guest has its own,
        //   first Options', put back as a profile gets up.
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
            if (op == "holder")
                return word(holder(n));
            if (op == "badge")
            {
                const std::string who = holder(n);
                return word(who.empty() ? who : seatTag(n) + who);
            }
            if (op == "ink")
            {
                const uint32_t was = static_cast<uint32_t>(call.number(4));
                std::vector<uint8_t> rgba = {uint8_t(was), uint8_t(was >> 8), uint8_t(was >> 16), 255};
                for (const seat_colors::Game &game : seat_colors::kGames)
                    if (call.symbol(3) == game.game)
                        recolor(rgba, game.seats[static_cast<size_t>(n % 4)]);
                return number(int(rgba[0] | rgba[1] << 8 | rgba[2] << 16));
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
                const int except = call.size() > 3 ? static_cast<int>(call.number(3)) : -1;
                for (int row = 0; row < save::kProfiles; ++row)
                    if (row != except && profiles.here(row) && lower(profiles.names[static_cast<size_t>(row)]) == name)
                        return number(1);
                return number(0);
            }
            if (op == "seat_of")
            {
                for (int seat = 0; seat < kPlayers; ++seat)
                    if (save::seatProfile(seat) == n)
                        return number(seat);
                return number(-1);
            }
            if (op == "rename")
            {
                if (profiles.here(n))
                    save::nameProfile(n, call.text(3));
                return {};
            }
            if (op == "delete")
            {
                save::deleteProfile(n);
                return {};
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
            if (op == "joins")
            {
                setMenuJoins(n != 0);
                return {};
            }
            if (op == "drop")
            {
                save::signOut(n);
                dropSeat(n);
                return {};
            }
            if (op == "pref")
                return number(call.symbol(3) == "speed" ? seatSpeed(n) : seatLength(n));
            if (op == "set_pref")
            {
                if (const auto profile = save::seatProfile(n))
                    save::keepProfilePref(*profile, call.symbol(3), static_cast<int>(call.number(4)));
                return {};
            }
            if (op == "shown")
            {
                const int tenths = n;
                return word(std::to_string(tenths / 10) + "." + std::to_string(tenths % 10));
            }
            if (op == "lefty" && n >= 0 && n < kPlayers)
            {
                const bool now = call.number(3) != 0.0f;
                const auto profile = save::seatProfile(n);
                const std::string ownKey = "lefty p" + std::to_string(n + 1);
                const auto keepOwn = [&](bool on) { save::keepState("guest", ownKey, on ? "1" : "0"); };
                const std::string kept = save::state("guest", ownKey);
                bool own = kept == "1", out = now;
                if (!s_leftySeen[n] || profile != s_leftyFor[n])
                {
                    // Options' own, taken while a guest holds the seat.
                    if (kept.empty() && (s_leftySeen[n] ? !s_leftyFor[n] : !profile))
                        keepOwn(own = now);
                    s_leftySeen[n] = true;
                    s_leftyFor[n] = profile;
                    out = profile ? save::profilePref(*profile, "lefty").value_or(own) != 0 : own;
                }
                else if (now != s_leftyWas[n])
                {
                    if (profile)
                        save::keepProfilePref(*profile, "lefty", now ? 1 : 0);
                    else
                        keepOwn(now);
                }
                s_leftyWas[n] = out;
                return number(out ? 1 : 0);
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
        seatPosters();
        // Profiles' (dta/profiles.dta, patched with the menus, which hold it).
        // The splash takes only Start, which picks a profile (main_menu.dta).
        locale::add("ghc_press_start", "PRESS START");
        locale::add("ghc_join", "press START");
        locale::add("ghc_profile_help", "PROFILE");
        locale::add("ghc_choose_profile", "CHOOSE PROFILE");
        locale::add("ghc_profile_p0", "P1 PROFILE");
        locale::add("ghc_profile_p1", "P2 PROFILE");
        locale::add("ghc_name_profile", "NAME YOUR PROFILE");
        locale::add("ghc_new_profile", "new profile");
        locale::add("ghc_guest", "guest");
        locale::add("ghc_profile_change", "CHANGE PROFILE");
        locale::add("ghc_profile_drop", "DROP OUT");
        locale::add("ghc_profile_lefty", "LEFTY FLIP");
        locale::add("ghc_profile_speed", "NOTE SPEED");
        locale::add("ghc_profile_length", "HWY LENGTH");
        locale::add("ghc_rename", "RENAME");
        locale::add("ghc_delete", "DELETE");
        locale::add("ghc_delete_profile", "DELETE PROFILE");
        locale::add("ghc_delete_profile_msg", "Are you sure you want to delete this profile?");
        locale::add("ghc_delete_band", "DELETE BAND");
        locale::add("ghc_delete_band_msg", "Are you sure you want to delete this band?");
        // GH2's own (locale.dta's no_profile and duplicate_profile), of profiles.
        locale::add("ghc_no_profile", "NO PROFILE");
        locale::add("ghc_no_profile_msg", "Please enter a PROFILE NAME.");
        locale::add("ghc_duplicate_profile", "DUPLICATE PROFILE");
        locale::add("ghc_duplicate_profile_msg",
                    "The PROFILE NAME you entered already exists. Please enter a different PROFILE NAME.");
        script::patchUi(kPlayersDta);
        // Multiplayer's help bar (players.dta).
        locale::add("ghc_players_reorder", "REORDER");
    }
}
