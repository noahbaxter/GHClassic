#include "ui/controllers.h"

#include "content/games.h"
#include "controllers_poster.h"
#include "disc/ark.h"
#include "dta.h"
#include "gh1/scene.h"
#include "host/devices.h"
#include "host/input.h"
#include "milo/milo.h"
#include "render/texture_store.h"
#include "script.h"
#include "ui/players.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace gh2
{
    namespace
    {
        using namespace input;

        // The rows: the buttons in the order a guitar is played, then the
        // two that have meters, then the row that puts the device back to
        // its built-in layout. The frets are kGreen to kOrange in order.
        struct Row
        {
            Action action;
            const char *name;
        };
        const Row kRows[] = {
            {kGreen, "GREEN"},          {kRed, "RED"},           {kYellow, "YELLOW"}, {kBlue, "BLUE"},
            {kOrange, "ORANGE"},        {kStrumUp, "STRUM UP"},  {kStrumDown, "STRUM DOWN"},
            {kStarPower, "SELECT"},     {kStart, "START"},       {kWhammy, "WHAMMY"}, {kTilt, "TILT"},
        };
        constexpr int kFrets = kOrange - kGreen + 1;
        constexpr int kActionRows = static_cast<int>(sizeof kRows / sizeof kRows[0]);
        constexpr int kRowCount = kActionRows + 1; // and Reset
        constexpr float kHoldToLeave = 3.0f;
        constexpr int kChips = 5;
        constexpr const char *kPosterScene = "ui/gen/lag.milo_ps2";

        // The Rat Cellar's TV in the corner (small1_geom: the PS2's channels
        // the venue's TV never shows but one of), its frame and each
        // channel's frames, flipped through while it plays, and Harmonix's
        // full-size colour bars (bars.milo) while a control is awaited. Left
        // out: the channels of photos of people.
        constexpr const char *kTvScene = "world/small1/og/gen/small1_geom.milo_ps2";
        constexpr const char *kBarsScene = "ui/gen/bars.milo_ps2";
        const std::vector<std::vector<const char *>> kChannels = {
            {"tv_good_rocker_1.tex", "tv_good_rocker_2.tex"},
            {"tv_good_city_fire_1.tex", "tv_good_city_fire_2.tex"},
            {"tv_good_anarchy.tex"},
            {"tv_good_bomb_big.tex", "tv_good_bomb_small.tex"},
            {"tv_bad_skull1.tex", "tv_bad_skull2.tex"},
            {"tv_good_equalizer.tex"},
        };
        constexpr const char *kTvFrame = "tv_border.tex";
        constexpr const char *kTestCard = "harmonix_splash.tex";
        constexpr uint64_t kFrameMs = 250;

        // What the screen shows, rebuilt each poll; the game thread's own.
        int s_focus = 0;
        std::vector<DeviceView> s_views;
        DeviceView s_shown;
        bool s_rebinding = false;
        int s_first = 0;     // the device in the first chip: the row scrolls to keep the one shown in view
        // The TV: its channel, and what was held last poll, for each new
        // press to change channel.
        size_t s_channel = 0;
        std::array<bool, kActionCount> s_wasHeld{};
        DeviceId s_heldOn; // the device those were read from
        std::string s_drawn; // what was last painted, to paint only on a change

        bool keyboard(const DeviceView &v)
        {
            return v.kind == DeviceKind::kKeyboard;
        }

        void refresh()
        {
            s_views = deviceViews();
            const DeviceId id = touchedDevice();
            s_shown = {};
            int at = 0;
            for (size_t i = 0; i < s_views.size(); ++i)
                if (s_views[i].id == id)
                {
                    s_shown = s_views[i];
                    at = static_cast<int>(i);
                }
            s_first = std::clamp(s_first, at - kChips + 1, at);
            s_first = std::clamp(s_first, 0, std::max(0, static_cast<int>(s_views.size()) - kChips));
            s_rebinding = rebinding();
        }

        // The device in chip i, null past the last.
        const DeviceView *inChip(int i)
        {
            const int at = s_first + i;
            return i >= 0 && i < kChips && at < static_cast<int>(s_views.size()) ? &s_views[static_cast<size_t>(at)]
                                                                                   : nullptr;
        }

        void move(int by)
        {
            s_focus = (s_focus + by + kRowCount) % kRowCount;
        }

        void nextDevice(int by)
        {
            const int count = static_cast<int>(s_views.size());
            for (int i = 0; i < count; ++i)
                if (s_views[static_cast<size_t>(i)].id == s_shown.id)
                {
                    showDevice(s_views[static_cast<size_t>((i + by + count) % count)].id);
                    return;
                }
            if (!s_views.empty())
                showDevice(s_views.front().id);
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        std::string title(const DeviceView &v)
        {
            return upper(v.label) + (v.number > 0 ? "  " + std::to_string(v.number) : "");
        }

        // A device's own name cut to fit a chip: its layout's or SDL's name
        // without the filler (DualSense Wireless Controller is DUALSENSE
        // CONTROLLER), at most 20 letters.
        std::string shortName(const DeviceView &v)
        {
            if (keyboard(v))
                return "KEYBOARD";
            std::string words, word;
            std::istringstream in(upper(v.label));
            while (in >> word)
                if (word != "WIRELESS" && word != "(R)" && word != "INC.")
                    words += (words.empty() ? "" : " ") + word;
            if (words.empty())
                words = upper(kindName(v.kind));
            constexpr size_t kMost = 20;
            if (words.size() > kMost)
                words = words.substr(0, words.find_last_of(' ', kMost) == std::string::npos
                                            ? kMost
                                            : words.find_last_of(' ', kMost));
            return words;
        }

        // A chip: the device's seat, short name and number among its model's.
        std::string chip(int i)
        {
            const DeviceView *v = inChip(i);
            if (!v)
                return "";
            return (v->seat >= 0 ? seatTag(v->seat) : std::string()) + shortName(*v) +
                   (v->number > 0 ? " " + std::to_string(v->number) : "");
        }

        bool held(int row)
        {
            return row >= 0 && row < kActionRows && s_shown.amount[static_cast<size_t>(kRows[row].action)] > kHeld;
        }

        bool unbound(int row)
        {
            return row >= 0 && row < kActionRows && s_shown.bound[static_cast<size_t>(kRows[row].action)].empty();
        }

        // How a row shows: held on the device shown, waiting for its control,
        // picked, or with nothing bound.
        const char *state(int row)
        {
            if (s_rebinding && row == s_focus)
                return "waiting";
            if (held(row))
                return "held";
            if (row == s_focus)
                return "picked";
            if (unbound(row))
                return "unset";
            return "plain";
        }

        // A row's fret, 0 green to 4 orange, else kFrets.
        int fretOf(int row)
        {
            const bool fret = row >= 0 && row < kActionRows && kRows[row].action >= kGreen && kRows[row].action <= kOrange;
            return fret ? kRows[row].action - kGreen : kFrets;
        }

        int rowOf(const std::string &action)
        {
            for (int row = 0; row < kActionRows; ++row)
                if (action == kActions[kRows[row].action].name)
                    return row;
            return -1;
        }

        std::string read(int row)
        {
            if (row >= kActionRows)
                return "";
            if (s_rebinding && row == s_focus)
                return "PRESS";
            const std::string &bound = s_shown.bound[static_cast<size_t>(kRows[row].action)];
            return bound.empty() ? "NOT SET" : bound;
        }

        std::string text(const std::string &what, int i)
        {
            if (what == "title")
                return title(s_shown);
            if (what == "chip")
                return chip(i);
            if (what == "before")
                return s_first > 0 ? "<" : "";
            if (what == "after")
                return s_first + kChips < static_cast<int>(s_views.size()) ? ">" : "";
            if (what == "row")
                return i < kActionRows ? kRows[i].name : "RESET TO DEFAULT";
            if (what == "read")
                return read(i);
            return "";
        }

        // Everything painted, so a poll paints only when something changed.
        std::string drawn()
        {
            std::string all = std::to_string(s_focus) + (s_rebinding ? "r" : "") + (keyboard(s_shown) ? "k" : "");
            for (const char *what : {"before", "after"})
                all += text(what, 0) + "|";
            for (int i = 0; i < kChips; ++i)
                all += chip(i) + "|";
            for (int row = 0; row < kRowCount; ++row)
                all += read(row) + "|";
            for (float a : s_shown.amount)
                all += a > kHeld ? "1" : "0";
            for (const DeviceView &v : s_views)
                all += v.id == s_shown.id ? "*" : ".";
            return all;
        }

        // Each new press on the device shown changes the TV's channel.
        void watchPresses()
        {
            bool pressed = false;
            for (size_t a = 0; a < s_wasHeld.size(); ++a)
            {
                const bool held = s_shown.amount[a] > kHeld;
                pressed = pressed || (held && !s_wasHeld[a] && s_heldOn == s_shown.id);
                s_wasHeld[a] = held;
            }
            s_heldOn = s_shown.id;
            if (!pressed)
                return;
            s_channel = (s_channel + 1 + SDL_rand(static_cast<Sint32>(kChannels.size() - 1))) % kChannels.size();
        }

        // The texture the TV shows now.
        const char *tv()
        {
            if (s_rebinding)
                return kTestCard;
            const auto &frames = kChannels[s_channel];
            return frames[(SDL_GetTicks() / kFrameMs) % frames.size()];
        }

        // {ghc_controllers poll}: the keyboard's keys acted on, and red held
        // long enough to leave. paint when what shows changed, leave to go
        // back, else same.
        std::string poll()
        {
            refresh();
            watchPresses();
            for (int key : takeScreenKeys())
            {
                if (s_rebinding)
                {
                    if (key == SDL_SCANCODE_ESCAPE)
                        cancelRebind();
                    continue;
                }
                switch (key)
                {
                case SDL_SCANCODE_UP:
                    move(-1);
                    break;
                case SDL_SCANCODE_DOWN:
                    move(1);
                    break;
                case SDL_SCANCODE_RETURN:
                case SDL_SCANCODE_KP_ENTER:
                    if (s_focus >= kActionRows)
                        resetBindings(s_shown.id);
                    else if (!s_shown.id.none())
                        startRebind(s_shown.id, kRows[s_focus].action);
                    break;
                case SDL_SCANCODE_DELETE:
                case SDL_SCANCODE_BACKSPACE:
                    if (s_focus < kActionRows)
                        clearBinding(s_shown.id, kRows[s_focus].action);
                    break;
                case SDL_SCANCODE_LEFT:
                    nextDevice(-1);
                    break;
                case SDL_SCANCODE_RIGHT:
                case SDL_SCANCODE_TAB:
                    nextDevice(1);
                    break;
                case SDL_SCANCODE_ESCAPE:
                    return "leave";
                default:
                    break;
                }
                refresh();
            }
            if (!s_rebinding && redHeldSeconds() >= kHoldToLeave)
                return "leave";
            const std::string now = drawn();
            if (now == s_drawn)
                return "same";
            s_drawn = now;
            return "paint";
        }

        script::Node symbolNode(const script::Call &call, const std::string &text)
        {
            return {script::symbol(call.rdram, call.ctx, call.runtime, text), script::kSymbol};
        }

        // {ghc_controllers step <0|1>}: the screen came up or went.
        // {ghc_controllers poll}: see poll().
        // {ghc_controllers text <title|chip|before|after|row|read> [i]}:
        // title is the device shown by name; chip i the device in that chip
        // of the row, which scrolls, and before and after a < or > while
        // more are off either end.
        // {ghc_controllers rows}, {ghc_controllers chips}: how many.
        // {ghc_controllers focus}: the row picked.
        // {ghc_controllers state <row>}: see state().
        // {ghc_controllers fret <row>}: its fret, 0 green to 4 orange, else 5.
        // {ghc_controllers row_of <action>}: that action's row.
        // {ghc_controllers chip_on <i>}: chip i holds the device shown.
        // {ghc_controllers held <action>}: that action, 0 to 1.
        // {ghc_controllers fret_held <fret>}: that fret, 0 green to 4 orange.
        // {ghc_controllers rebinding}, {ghc_controllers keyboard}: whether a
        // control is awaited, and whether the device shown is the keyboard.
        // {ghc_controllers explorer}: whether the tutorial's guitar is the
        // 360's X-plorer art (gh2x/textures.h), else the PS2's SG.
        // {ghc_controllers tv}: the texture the corner's TV shows now.
        // {ghc_controllers blink}: 0 to 1 and back twice a second while a
        // control is awaited, else 0.
        // {ghc_controllers leaving}: how far red is held toward leaving, 0
        // to 1; 0 on the keyboard, which leaves by Esc.
        script::Node command(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "step")
            {
                const bool on = call.number(2) != 0.0f;
                setControllersScreen(on);
                if (on)
                {
                    s_focus = 0;
                    s_drawn.clear();
                }
                return {};
            }
            if (op == "poll")
                return symbolNode(call, poll());
            const int i = static_cast<int>(call.number(2));
            if (op == "text")
                return symbolNode(call, text(call.symbol(2), static_cast<int>(call.number(3))));
            if (op == "rows")
                return {static_cast<uint32_t>(kRowCount), script::kInt};
            if (op == "chips")
                return {static_cast<uint32_t>(kChips), script::kInt};
            if (op == "focus")
                return {static_cast<uint32_t>(s_focus), script::kInt};
            if (op == "state")
                return symbolNode(call, state(i));
            if (op == "fret")
                return {static_cast<uint32_t>(fretOf(i)), script::kInt};
            if (op == "row_of")
                return {static_cast<uint32_t>(rowOf(call.symbol(2))), script::kInt};
            if (op == "fret_held")
                return script::floatNode(i >= 0 && i < kFrets ? s_shown.amount[static_cast<size_t>(kGreen + i)] : 0.0f);
            if (op == "chip_on")
            {
                const DeviceView *v = inChip(i);
                return {v && v->id == s_shown.id ? 1u : 0u, script::kInt};
            }
            if (op == "explorer")
                return {games::disc("gh2x") ? 1u : 0u, script::kInt};
            if (op == "rebinding")
                return {s_rebinding ? 1u : 0u, script::kInt};
            if (op == "keyboard")
                return {keyboard(s_shown) ? 1u : 0u, script::kInt};
            if (op == "tv")
                return symbolNode(call, tv());
            if (op == "blink")
                return script::floatNode(
                    s_rebinding ? 0.5f - 0.5f * std::cos(static_cast<float>(SDL_GetTicks() % 500u) / 500.0f * 6.2831853f)
                                : 0.0f);
            if (op == "leaving")
                return script::floatNode(s_rebinding || keyboard(s_shown)
                                             ? 0.0f
                                             : std::min(redHeldSeconds() / kHoldToLeave, 1.0f));
            if (op == "held")
                for (int a = 0; a < kActionCount; ++a)
                    if (call.symbol(2) == kActions[a].name)
                        return script::floatNode(s_shown.amount[static_cast<size_t>(a)]);
            return {};
        }
    }

    void installControllers(PS2Runtime &)
    {
        script::addCommand("ghc_controllers", command);
        script::patchUi(kControllersDta);
        // The screen's black poster: a Tex of its own beside Calibrate Lag's
        // in that scene, which the screen's copy of it draws instead.
        textures::addBundled("controllers/poster", kControllersPoster, sizeof kControllersPoster);
        if (const auto disc = games::disc("gh2"))
            ark::addFix(*disc, kPosterScene, [](std::vector<uint8_t> &file)
                        {
                            auto scene = milo::read(file);
                            const auto at = scene ? milo::find(*scene, "syncoff_backdrop.tex") : std::nullopt;
                            if (!at)
                            {
                                std::cerr << "[controllers] no poster in " << kPosterScene << std::endl;
                                return;
                            }
                            const uint32_t id = textures::add({textures::kBundled, "controllers/poster", ""});
                            milo::add(*scene, "Tex", "ghc_ct_poster.tex",
                                      gh1::texWith(scene->bodies[*at], textures::standIn(id, 64u, 64u), true));
                            file = milo::write(*scene);
                        });
        // The TV's pictures, copied into the tutorial's scene the screen
        // draws in, either hand's.
        if (const auto disc = games::disc("gh2"))
            for (const char *tutorial : {"ui/gen/tut_script.milo_ps2", "ui/gen/tut_script_lefty.milo_ps2"})
                ark::addFix(*disc, tutorial, [disc = *disc](std::vector<uint8_t> &file)
                            {
                                auto scene = milo::read(file);
                                if (!scene)
                                    return;
                                std::vector<const char *> wanted = {kTvFrame, kTestCard};
                                for (const auto &frames : kChannels)
                                    wanted.insert(wanted.end(), frames.begin(), frames.end());
                                for (const char *from : {kTvScene, kBarsScene})
                                {
                                    const auto bytes = ark::readFile(disc, from);
                                    const auto source = bytes ? milo::read(*bytes) : std::nullopt;
                                    if (!source)
                                    {
                                        std::cerr << "[controllers] no TV art in " << from << std::endl;
                                        continue;
                                    }
                                    for (const char *name : wanted)
                                        if (const auto at = milo::find(*source, name); at && !milo::find(*scene, name))
                                            milo::add(*scene, "Tex", name, source->bodies[*at]);
                                }
                                file = milo::write(*scene);
                            });
    }
}
