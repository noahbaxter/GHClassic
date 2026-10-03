#include "content/campaigns.h"

#include "content/locale.h"
#include "content/outfits.h"
#include "content/setlists.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "guest.h"
#include "hook.h"
#include "save/save.h"
#include "script.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>

namespace gh2::campaigns
{
    namespace
    {
        struct Campaign
        {
            std::string game;
            std::optional<size_t> front;
        };

        // The config's arrays that differ by game, each a path of keys from
        // the root. The rest is the game disc's for every campaign: songs
        // holds every game's, and the objects, cheats and sound setup are
        // read once at boot.
        const std::vector<std::vector<const char *>> kConfig = {
            {"campaign"}, {"store"}, {"guitars"}, {"tips"}, {"venues"}, {"characters"}, {"synth", "metamusic"},
        };
        constexpr const char *kRootConfig = "config/gh2.dta";
        constexpr const char *kUi = "ui/ui.dta";
        // Hmx::Object's vtable entry for SetTypeDef: a this adjustment
        // (s16), then the function.
        constexpr uint32_t kSetTypeDefSlot = 0x58u;
        constexpr uint32_t kCommand = 0x11u; // a DataNode's type: {...}
        constexpr uint32_t kAllAccess = 0x80u; // Campaign's, the unlock-all cheat
        constexpr uint32_t kVenueProvider = 0xa0u; // GameConfig's VenueProvider

        const Addresses *s_addresses = nullptr;
        std::vector<Campaign> s_campaigns = {{"gh2", std::nullopt}};
        size_t s_active = 0u;

        struct Pending
        {
            size_t index;
            std::string next; // the screen to go to once switched, or none
        };
        std::optional<Pending> s_pending;
        bool s_toSwitch = false; // the screen being gone to is the switch's

        // A DataArray: its nodes (+0), each a value and a type, their count
        // (+8) and its references (+0xa).
        struct Guest
        {
            const script::Call &call;

            uint32_t run(uint32_t function, std::initializer_list<uint32_t> args) const
            {
                return static_cast<uint32_t>(call.runtime->callGuestFunction(call.rdram, call.ctx, function, args));
            }
            int size(uint32_t array) const { return load<int16_t>(call.rdram, array + 8u); }
            uint32_t node(uint32_t array, int i) const { return load<uint32_t>(call.rdram, array) + 8u * i; }
            uint32_t type(uint32_t array, int i) const { return load<uint32_t>(call.rdram, node(array, i) + 4u); }
            uint32_t value(uint32_t array, int i) const { return load<uint32_t>(call.rdram, node(array, i)); }
            // Its first node's Symbol, or "" when it starts on none.
            std::string key(uint32_t array) const
            {
                if (size(array) == 0 || type(array, 0) != script::kSymbol)
                    return "";
                return reinterpret_cast<const char *>(getMemPtr(call.rdram, value(array, 0)));
            }
            // The array under `array` that starts on `name`, or 0.
            uint32_t find(uint32_t array, const std::string &name) const
            {
                for (int i = 0; i < size(array); ++i)
                    if (type(array, i) == script::kArray && key(value(array, i)) == name)
                        return value(array, i);
                return 0u;
            }
            void hold(uint32_t array, int by) const
            {
                store<int16_t>(call.rdram, array + 0xau, static_cast<int16_t>(load<int16_t>(call.rdram, array + 0xau) + by));
            }
            // As a DataNode lets go of its array.
            void release(uint32_t array) const
            {
                hold(array, -1);
                if (load<int16_t>(call.rdram, array + 0xau) == 0)
                    run(s_addresses->dataArrayDtor, {array, 3u});
            }
            uint32_t readFile(const std::string &path) const
            {
                const uint32_t text = run(s_addresses->builtinNew, {static_cast<uint32_t>(path.size()) + 1u});
                std::memcpy(getMemPtr(call.rdram, text), path.c_str(), path.size() + 1u);
                const uint32_t array = run(s_addresses->dataReadFile, {text});
                run(s_addresses->builtinDelete, {text});
                return array;
            }
        };

        // `to` takes `from`'s contents. An array under `to` that `from` has
        // too, by its key or else by its place, stays the object it was and
        // takes its counterpart's contents the same way. One `from` lacks
        // keeps a reference, so a pointer the game still holds to it stays
        // good.
        void sync(const Guest &guest, uint32_t to, uint32_t from, uint32_t scratch)
        {
            const int count = guest.size(from), before = guest.size(to);
            std::map<std::string, uint32_t> keyed;
            std::vector<uint32_t> placed(static_cast<size_t>(before), 0u);
            for (int i = 0; i < before; ++i)
            {
                if (guest.type(to, i) != script::kArray)
                    continue;
                const uint32_t array = guest.value(to, i);
                guest.hold(array, 1);
                placed[static_cast<size_t>(i)] = array;
                if (const std::string key = guest.key(array); !key.empty())
                    keyed.emplace(key, array);
            }
            std::vector<uint32_t> kept(static_cast<size_t>(count), 0u);
            for (int i = 0; i < count; ++i)
            {
                if (guest.type(from, i) != script::kArray)
                    continue;
                const std::string key = guest.key(guest.value(from, i));
                if (!key.empty())
                {
                    if (const auto it = keyed.find(key); it != keyed.end())
                    {
                        kept[static_cast<size_t>(i)] = it->second;
                        keyed.erase(it);
                    }
                }
                else if (i < before && placed[static_cast<size_t>(i)] != 0u && guest.key(placed[static_cast<size_t>(i)]).empty())
                    kept[static_cast<size_t>(i)] = placed[static_cast<size_t>(i)];
            }
            for (int i = 0; i < count; ++i)
                if (kept[static_cast<size_t>(i)] != 0u)
                    sync(guest, kept[static_cast<size_t>(i)], guest.value(from, i), scratch);
            guest.run(s_addresses->dataArrayResize, {to, static_cast<uint32_t>(count)});
            for (int i = 0; i < count; ++i)
            {
                const uint32_t array = kept[static_cast<size_t>(i)];
                if (array == 0u)
                {
                    guest.run(s_addresses->dataNodeAssign, {guest.node(to, i), guest.node(from, i)});
                    continue;
                }
                store<uint32_t>(guest.call.rdram, scratch, array);
                store<uint32_t>(guest.call.rdram, scratch + 4u, script::kArray);
                guest.run(s_addresses->dataNodeAssign, {guest.node(to, i), scratch});
                guest.hold(array, -1);
            }
        }

        void syncConfig(const Guest &guest, uint32_t fresh)
        {
            const uint32_t live = guest.run(s_addresses->systemConfig, {});
            const uint32_t scratch = guest.run(s_addresses->builtinNew, {8u});
            for (const auto &path : kConfig)
            {
                uint32_t from = fresh, to = live;
                for (const char *key : path)
                {
                    from = from ? guest.find(from, key) : 0u;
                    to = to ? guest.find(to, key) : 0u;
                }
                if (from == 0u || to == 0u)
                    std::cerr << "[campaign] no " << path.back() << " in the config" << std::endl;
                else
                    sync(guest, to, from, scratch);
            }
            guest.run(s_addresses->builtinDelete, {scratch});
        }

        // The scene a panel made in `init` loads, {new Class name (file
        // x.milo) ...}, as the archive holds it: ui/gen/x.milo_ps2.
        std::string sceneOf(const Guest &guest, uint32_t init, const std::string &panel)
        {
            for (int i = 0; i < guest.size(init); ++i)
            {
                if (guest.type(init, i) != kCommand)
                    continue;
                const uint32_t command = guest.value(init, i);
                if (guest.key(command) != "new" || guest.size(command) < 3 || guest.type(command, 2) != script::kSymbol ||
                    panel != reinterpret_cast<const char *>(getMemPtr(guest.call.rdram, guest.value(command, 2))))
                    continue;
                const uint32_t file = guest.find(command, "file");
                if (file == 0u || guest.size(file) < 2)
                    return "";
                // A string node holds an array whose nodes are its characters.
                const uint32_t text = guest.type(file, 1) == script::kSymbol ? guest.value(file, 1)
                                                                              : load<uint32_t>(guest.call.rdram, guest.value(file, 1));
                return std::string("ui/gen/") + reinterpret_cast<const char *>(getMemPtr(guest.call.rdram, text)) + "_ps2";
            }
            return "";
        }

        void retypeUi(const Guest &guest, uint32_t init, const Campaign &from, const Campaign &to)
        {
            for (int i = 0; i < guest.size(init); ++i)
            {
                if (guest.type(init, i) != kCommand)
                    continue;
                const uint32_t command = guest.value(init, i);
                // {foreach $p (pause_panel ... helpbar) {$p load}}: the panels
                // kept loaded from boot. Each whose scene is another file
                // now is let go, and the command run again loads it. The
                // help bar's, which is built at boot (ui/help_bar.cpp), is
                // the same one, and stays.
                if (guest.key(command) == "foreach" && guest.size(command) == 4 && guest.type(command, 2) == script::kArray &&
                    guest.type(command, 3) == kCommand && guest.size(guest.value(command, 3)) == 2 &&
                    guest.type(guest.value(command, 3), 1) == script::kSymbol &&
                    std::strcmp(reinterpret_cast<const char *>(getMemPtr(
                                    guest.call.rdram, guest.value(guest.value(command, 3), 1))),
                                "load") == 0)
                {
                    const uint32_t panels = guest.value(command, 2);
                    std::string unload;
                    for (int p = 0; p < guest.size(panels); ++p)
                    {
                        if (guest.type(panels, p) != script::kSymbol)
                            continue;
                        const std::string name = reinterpret_cast<const char *>(getMemPtr(guest.call.rdram, guest.value(panels, p)));
                        const std::string scene = sceneOf(guest, init, name);
                        ark::front(from.front);
                        const auto was = ark::origin(scene);
                        ark::front(to.front);
                        if (!scene.empty() && was != ark::origin(scene))
                            unload += "{" + name + " unload}\n";
                    }
                    script::run(guest.call.rdram, guest.call.ctx, guest.call.runtime, unload);
                    guest.run(s_addresses->dataNodeEvaluate, {guest.node(init, i)});
                    continue;
                }
                // {new Class name (...)...}, the arrays its type definition.
                if (guest.key(command) != "new" || guest.size(command) < 4 || guest.type(command, 2) != script::kSymbol ||
                    guest.type(command, 3) != script::kArray)
                    continue;
                const uint32_t object = guest.run(s_addresses->dataNodeGetObj, {guest.node(command, 2), command});
                if (object == 0u)
                    continue;
                const uint32_t vtable = load<uint32_t>(guest.call.rdram, object);
                const int16_t delta = load<int16_t>(guest.call.rdram, vtable + kSetTypeDefSlot);
                guest.run(load<uint32_t>(guest.call.rdram, vtable + kSetTypeDefSlot + 4u),
                          {object + static_cast<uint32_t>(static_cast<int32_t>(delta)), command});
            }
        }


        bool switchTo(const script::Call &call, size_t index)
        {
            if (index == s_active)
                return false;
            const Guest guest{call};
            const size_t before = s_active;
            std::string laps;
            auto last = std::chrono::steady_clock::now();
            const auto lap = [&](const char *step)
            {
                const auto now = std::chrono::steady_clock::now();
                laps += std::string(" ") + step + " " +
                        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count());
                last = now;
            };
            // Both files as the new campaign's archive gives them, before
            // anything changes.
            ark::front(s_campaigns[index].front);
            const uint32_t config = guest.readFile(kRootConfig);
            lap("config");
            const uint32_t ui = guest.readFile(kUi);
            lap("ui");
            const uint32_t init = ui ? guest.find(ui, "init") : 0u;
            if (config == 0u || init == 0u)
            {
                std::cerr << "[campaign] cannot read " << s_campaigns[index].game << "'s " << (config ? kUi : kRootConfig)
                          << std::endl;
                for (const uint32_t read : {config, ui})
                    if (read != 0u)
                        guest.release(read);
                ark::front(s_campaigns[before].front);
                return false;
            }
            // The career as it stands goes into the save's sections first.
            ark::front(s_campaigns[before].front);
            save::leaveGame(call.rdram, call.ctx, call.runtime);
            lap("leave");
            ark::front(s_campaigns[index].front);
            s_active = index;
            outfits::campaignChanged();
            syncConfig(guest, config);
            // GameConfig's list of the config's venues, which its constructor
            // (0x125c98) builds once: the career's map opens on the last
            // venue unlocked before the first in it that is not
            // (GameConfig::SetCareerVenue, 0x126c50).
            guest.run(s_addresses->venueProviderInitData,
                      {load<uint32_t>(call.rdram, s_addresses->theGameConfig) + kVenueProvider, 0u});
            lap("sync");
            retypeUi(guest, init, s_campaigns[before], s_campaigns[index]);
            lap("retype");
            // What the live tree and the objects now hold stays; the rest of
            // each read goes.
            guest.release(config);
            guest.release(ui);
            lap("release");
            script::patchUiAgain(call.rdram, call.ctx, call.runtime);
            lap("patches");
            locale::reload(call.rdram, call.ctx, call.runtime);
            lap("strings");
            // The Campaign where it stands, so every pointer to it holds. All
            // access goes back on after the load, which it would refuse.
            const uint32_t campaign = load<uint32_t>(call.rdram, s_addresses->theCampaign);
            const uint32_t allAccess = load<uint32_t>(call.rdram, campaign + kAllAccess);
            guest.run(s_addresses->campaignDtor, {campaign, 0u});
            guest.run(s_addresses->campaignCtor, {campaign});
            lap("campaign");
            save::enterGame(s_campaigns[index].game, call.rdram, call.ctx, call.runtime);
            store<uint32_t>(call.rdram, campaign + kAllAccess, allAccess);
            lap("enter");
            std::cerr << "[campaign] " << s_campaigns[before].game << " to " << s_campaigns[index].game << ", ms:" << laps
                      << std::endl;
            return true;
        }

        // The first {$variable set_character <outfit> ...} under `node`: the
        // second player's default, which main.dta's main_panel sets.
        const dtb::Node *setCharacter(const dtb::Node &node)
        {
            if (node.nodes.size() > 2u && node.nodes[0].type == dtb::kVar && node.nodes[1].text == "set_character")
                return &node.nodes[2];
            for (const dtb::Node &child : node.nodes)
                if (const dtb::Node *found = setCharacter(child))
                    return found;
            return nullptr;
        }

        // The active game's own, read off its disc: rockabill1 in GH2, goth2
        // in the 80s.
        std::string secondCharacter()
        {
            static std::map<size_t, std::string> s_found;
            if (const auto it = s_found.find(s_active); it != s_found.end())
                return it->second;
            const auto front = s_campaigns[s_active].front;
            dtb::Macros macros;
            const auto main = dtb::read("ui/main.dta", macros, [front](const std::string &path) { return ark::readFront(front, path); });
            const dtb::Node *outfit = main ? setCharacter(*main) : nullptr;
            return s_found[s_active] = outfit ? outfit->text : "punk1";
        }

        // How far band `slot` is through `game`'s career, 0 to 100: its
        // career songs passed on the difficulty that has passed most of
        // them, as a share of the career's.
        int progress(const std::string &game, int slot)
        {
            const std::vector<std::string> career = setlists::careerSongs(game);
            size_t most = 0u;
            for (const std::set<std::string> &passed : save::passedSongs(game, slot))
            {
                size_t count = 0u;
                for (const std::string &song : career)
                    count += passed.count(song);
                most = std::max(most, count);
            }
            return career.empty() ? 0 : static_cast<int>(most * 100u / career.size());
        }

        // Whether `game`'s store sells `item`: named in any of store.dta's
        // lists, read off its disc.
        bool sells(const Campaign &campaign, const std::string &item)
        {
            static std::map<std::string, std::set<std::string>> s_sold;
            auto found = s_sold.find(campaign.game);
            if (found == s_sold.end())
            {
                found = s_sold.emplace(campaign.game, std::set<std::string>()).first;
                const auto front = campaign.front;
                dtb::Macros macros;
                const auto store =
                    dtb::read("config/store.dta", macros, [front](const std::string &path) { return ark::readFront(front, path); });
                for (const dtb::Node &list : store ? store->nodes : std::vector<dtb::Node>())
                    for (size_t i = 1u; i < list.nodes.size(); ++i)
                        found->second.insert(list.nodes[i].type == dtb::kArray && !list.nodes[i].nodes.empty()
                                                 ? list.nodes[i].nodes[0].text
                                                 : list.nodes[i].text);
            }
            return found->second.count(item) != 0u;
        }

        std::optional<size_t> indexOf(const std::string &game)
        {
            for (size_t i = 0; i < s_campaigns.size(); ++i)
                if (s_campaigns[i].game == game)
                    return i;
            return std::nullopt;
        }

        script::Node campaignsCommand(const script::Call &call)
        {
            const std::string op = call.symbol(1);
            if (op == "active")
                return {script::symbol(call.rdram, call.ctx, call.runtime, active()), script::kSymbol};
            if (op == "has")
                return {indexOf(call.symbol(2)) ? 1u : 0u, script::kInt};
            if (op == "second_character")
                return {script::symbol(call.rdram, call.ctx, call.runtime, secondCharacter()), script::kSymbol};
            if (op == "progress")
            {
                // The career being played as it stands, not as last saved.
                if (call.symbol(2) == active())
                    save::leaveGame(call.rdram, call.ctx, call.runtime);
                return {static_cast<uint32_t>(progress(call.symbol(2), static_cast<int>(call.number(3)))), script::kInt};
            }
            if (op == "count")
                return {static_cast<uint32_t>(s_campaigns.size()), script::kInt};
            if (op == "owns")
            {
                // A game with no career here has nothing to earn it in.
                const auto index = indexOf(call.symbol(2));
                const std::string item = call.symbol(3);
                const bool owned = !index || !sells(s_campaigns[*index], item) || save::unlockedItem(call.symbol(2), item);
                return {owned ? 1u : 0u, script::kInt};
            }
            if (op == "switch")
            {
                const auto index = indexOf(call.symbol(2));
                if (!index || *index == s_active)
                    return {0u, script::kInt};
                s_pending = Pending{*index, call.size() > 3 ? call.symbol(3) : ""};
                return {1u, script::kInt};
            }
            std::cerr << "[campaign] usage: {campaigns active|count}, {campaigns has|switch <game>}" << std::endl;
            return {};
        }

        // A switch asked for from a script happens here, at the next
        // UIManager::Poll, with no script running: the screen that asked
        // takes a new script too. The band stays the one picked. One that
        // fails leaves the game as it was, at the main menu.
        struct PollTag;
        void onUiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_pending)
                return;
            const Pending pending = *s_pending;
            s_pending.reset();
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, "{set $ghc_band {campaign profile_slot}}");
            if (!switchTo(script::Call{rdram, ctx, runtime, 0u}, pending.index))
                script::run(rdram, ctx, runtime, "{ui goto_screen main_screen}");
            else
                script::run(rdram, ctx, runtime,
                            "{campaign set_profile_slot $ghc_band}" +
                                (pending.next.empty() ? std::string() : "{ui goto_screen " + pending.next + "}"));
            *ctx = saved;
        }
    }

    namespace
    {
        // The main menu's first showing, after the boot's load: where the
        // save says another game was the last played, by way of the screen
        // the switch happens on (campaigns.dta), so the game opens as that
        // one.
        struct GotoScreenTag;
        void onGotoScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static bool s_opened = false;
            static uint32_t screens[2] = {0u, 0u}; // main_screen, ghc_switch_screen
            const uint32_t screen = GPR_U32(ctx, 5);
            s_toSwitch = screen != 0u && screen == screens[1];
            if (s_opened || screen == 0u)
                return;
            const R5900Context saved = *ctx;
            const uint32_t names = script::parse(rdram, ctx, runtime, "main_screen ghc_switch_screen");
            for (uint32_t i = 0u; names != 0u && i < 2u; ++i)
                screens[i] = static_cast<uint32_t>(runtime->callGuestFunction(
                    rdram, ctx, s_addresses->dataNodeGetObj, {load<uint32_t>(rdram, names) + 8u * i, names}));
            const bool toMain = screens[0] != 0u && screens[0] == screen;
            const std::string last = toMain ? save::lastGame() : "";
            if (toMain && screens[1] != 0u && indexOf(last) && last != active())
                script::run(rdram, ctx, runtime, "{set $ghc_switch " + last + "} {set $ghc_switch_next main_screen}");
            *ctx = saved;
            if (!toMain)
                return;
            s_opened = true;
            if (screens[1] != 0u && indexOf(last) && last != active())
            {
                SET_GPR_U32(ctx, 5, screens[1]);
                s_toSwitch = true;
            }
        }

        // Leaving for a switch, the menu music fades out with the screen,
        // over its half second (GHScreen::AnimateTransition, 0x145e18):
        // MetaPanel holds the screen until the music is out
        // (MetaPanel::Exiting, 0x134918), and MetaMusic::Stop (0x21f668)
        // takes 1000 ms over its fade_rate (+0x44), a second as config has
        // it. LoadAndPlay (0x21ef78) reads the rate again.
        struct MetaMusicStopTag;
        void onMetaMusicStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (s_toSwitch)
                store<float>(rdram, GPR_U32(ctx, 4) + 0x44u, 2.0f);
        }
    }

    void add(const std::string &game, size_t front)
    {
        s_campaigns.push_back({game, front});
    }

    const std::string &active()
    {
        return s_campaigns[s_active].game;
    }

    std::vector<std::string> games()
    {
        std::vector<std::string> out;
        for (const Campaign &campaign : s_campaigns)
            out.push_back(campaign.game);
        return out;
    }

    const std::vector<std::string> &venues()
    {
        static std::map<size_t, std::vector<std::string>> s_found;
        if (const auto it = s_found.find(s_active); it != s_found.end())
            return it->second;
        const auto front = s_campaigns[s_active].front;
        dtb::Macros macros;
        const auto config = dtb::read(kRootConfig, macros, [front](const std::string &path) { return ark::readFront(front, path); });
        const dtb::Node *list = config ? dtb::find(*config, "venues") : nullptr;
        std::vector<std::string> &venues = s_found[s_active];
        for (size_t i = 1u; list && i < list->nodes.size(); ++i)
            venues.push_back(list->nodes[i].text);
        return venues;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::addCommand("campaigns", campaignsCommand);
        EntryHook<PollTag>::install(runtime, addresses.uiManagerPoll, onUiPoll);
        EntryHook<GotoScreenTag>::install(runtime, addresses.uiGotoScreen, onGotoScreen);
        EntryHook<MetaMusicStopTag>::install(runtime, addresses.metaMusicStop, onMetaMusicStop);
    }
}
