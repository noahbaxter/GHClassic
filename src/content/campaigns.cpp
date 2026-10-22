#include "content/campaigns.h"

#include "content/locale.h"
#include "content/outfits.h"
#include "content/setlists.h"
#include "disc/ark.h"
#include "formats/dtb.h"
#include "gameplay/note_engine/note_engine.h"
#include "guest.h"
#include "hook.h"
#include "save/save.h"
#include "script.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <unordered_map>

namespace gh2::campaigns
{
    namespace
    {
        struct Campaign
        {
            std::string game;
            std::optional<size_t> front;
            // Its files' own, read once and kept: each of kConfig's arrays,
            // 0 for one it lacks, and ui.dta's (init ...).
            std::vector<uint32_t> config = {};
            uint32_t init = 0u;
        };

        // The config's arrays that differ by game, each a path of keys from
        // the root. The rest is the game disc's for every campaign: songs
        // holds every game's, and the objects, cheats and sound setup are
        // read once at boot, but for the venues' types: each a world's
        // script, found by its name when the world loads.
        const std::vector<std::vector<const char *>> kConfig = {
            {"campaign"}, {"store"}, {"guitars"}, {"tips"}, {"venues"}, {"characters"}, {"synth", "metamusic"},
            {"objects", "WorldDir", "types"},
        };
        constexpr size_t kMetaMusic = 6u;  // kConfig's synth metamusic: the menus' music
        constexpr size_t kWorldTypes = 7u; // kConfig's objects WorldDir types: each venue's script
        // The menus: the main menu and what it opens on its own, the boot's
        // splash, the bands and the options, but not the options a song's
        // pause opens. Outside a career they are the theme's and the rest
        // are GH2's, the help bar, the card's prompts and the dialog they
        // show in among them; the files beside them too.
        const std::set<std::string> kMenuPanels = {
            "main_panel",           "main_screen",           "splash_panel",           "splash_screen",
            "cut_scene_screen",     "guitar_help_panel",     "guitar_help_screen",     "options_panel",
            "options_screen",       "audio_settings_panel",  "audio_settings_screen",  "video_settings_panel",
            "video_settings_screen", "mem_card_panel",       "mem_card_screen",        "bonus_material_panel",
            "bonus_material_screen", "play_bonus_video_screen", "credits_panel",       "credits_screen",
            "lag_panel",            "lag_screen",            "pscan_warning_panel",    "pscan_warning_screen",
            "pscan_switching_panel", "pscan_switch_screen",  "boot_pscan_warning_screen", "chooseprof_panel",
            "chooseprof_screen",    "nameprof_panel",        "nameprof_screen",        "manage_band_panel",
            "manage_band_screen",   "options_chooseprof_screen", "training_panel",     "training_screen",
            "tutorials_panel",      "tutorials_screen",
        };
        const std::vector<const char *> kMenuFiles = {
            "videos/intro.pss", "config/gen/credits.dtb", "ui/gen/list_credits.milo_ps2", "sfx/streams/credits.vgs",
        };
        // A song's venue lends its world: its rooms, crowd and lights, and
        // its venues' scripts (kWorldTypes).
        const std::vector<const char *> kVenueFiles = {"world/", "venues/"};
        // The in-song look lends the screens around the song: loading before
        // and after, pause and its settings, lose, the results and the high
        // scores. Their scenes as its scripts name them, and the
        // pieces beside them: the 80s' pause art is pause_lose_tex, its
        // loading words loading_word, and the results' photo picture_endgame.
        const std::set<std::string> kSongPanels = {
            "loading_panel",          "loading_screen",           "practice_loading_screen",
            "meta_loading_panel",     "meta_loading_screen",      "pause_panel",
            "pause_screen",           "pause_controller_panel",   "pause_controller_screen",
            "multi_pause_controller_panel", "multi_pause_controller_screen", "pract_pause_panel",
            "pract_pause_screen",     "practice_pause_controller_screen", "tut_pause_panel",
            "tut_pause_screen",       "tut_pause_controller_panel", "tut_pause_controller_screen",
            "lefty_restart_panel",    "lefty_restart_screen",     "pause_audio_settings_panel",
            "pause_audio_settings_screen", "pause_video_settings_panel", "pause_video_settings_screen",
            "lose_panel",             "lose_screen",              "endgame_panel",
            "endgame_screen",         "endgame_encore_panel",     "endgame_encore_screen",
            "endgame_stats_panel",    "endgame_stats_screen",     "highscore_screen",
            "practice_end_panel",     "practice_end_screen",
        };
        // highscore_panel is ours whichever game's (highscore_panel.dta), over
        // the look's scene.
        const std::vector<const char *> kSongFiles = {
            "ui/image/og/eng/gen/loading_word", "ui/gen/pause_settings.milo", "ui/gen/pause_lose_tex.milo",
            "ui/gen/picture_endgame.milo",      "ui/gen/highscore.milo",
        };
        // The highway lends its gems and strikeline, and the HUD's meters.
        const std::vector<const char *> kHighway = {"track/gen/track.milo_ps2", "hud/gen/"};
        constexpr size_t kTips = 3u;                                // kConfig's tips: the loading screen's
        constexpr const char *kRootConfig = "config/gh2.dta";
        constexpr const char *kUi = "ui/ui.dta";
        // Hmx::Object's vtable entry for SetTypeDef: a this adjustment
        // (s16), then the function.
        constexpr uint32_t kSetTypeDefSlot = 0x58u;
        constexpr uint32_t kAllAccess = 0x80u; // Campaign's, the unlock-all cheat
        constexpr uint32_t kVenueProvider = 0xa0u; // GameConfig's VenueProvider

        const Addresses *s_addresses = nullptr;
        std::vector<Campaign> s_campaigns = {{"gh2", std::nullopt}};
        size_t s_active = 0u;    // the campaign in front: its config, its career; GH2's outside a career
        size_t s_menus = 0u;     // whose menus are up: the active one's, or the theme's outside a career
        size_t s_theme = 0u;     // the menus picked for outside a career
        bool s_career = false;   // in a career, where everything is the active one's
        size_t s_venueFrom = 0u; // the campaign whose venues the next song plays in
        size_t s_lookFrom = 0u;  // whose screens around it it has
        size_t s_gemsFrom = 0u;  // and whose highway, which is lent apart
        std::atomic<bool> s_gh1Highway{false}; // that one is GH1's, for gh1Highway
        std::vector<std::string> s_menusLent, s_lookLent;

        struct Pending
        {
            size_t index;
            std::string next; // the screen to go to once switched, or none
            bool career;      // into its career, else its menus outside one
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
            void release(uint32_t array) const { script::release(call.rdram, call.ctx, call.runtime, array); }
            uint32_t clone(uint32_t array) const { return script::clone(call.rdram, call.ctx, call.runtime, array); }
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

        uint32_t under(const Guest &guest, uint32_t array, const std::vector<const char *> &path)
        {
            for (const char *key : path)
                array = array ? guest.find(array, key) : 0u;
            return array;
        }

        // The campaign's files read, as its layer in front gives them, and
        // what a switch takes of them kept, outside the game's heap
        // (script::park). False if either cannot be read.
        bool build(const Guest &guest, Campaign &campaign)
        {
            const uint32_t config = guest.readFile(kRootConfig);
            const uint32_t ui = config ? guest.readFile(kUi) : 0u;
            const uint32_t init = ui ? guest.find(ui, "init") : 0u;
            if (init != 0u)
            {
                for (const auto &path : kConfig)
                {
                    const uint32_t array = under(guest, config, path);
                    campaign.config.push_back(array ? script::park(guest.call.rdram, guest.call.runtime, array) : 0u);
                }
                campaign.init = script::park(guest.call.rdram, guest.call.runtime, init);
            }
            else
                std::cerr << "[campaign] cannot read " << campaign.game << "'s " << (config ? kUi : kRootConfig) << std::endl;
            for (const uint32_t read : {config, ui})
                if (read != 0u)
                    guest.release(read);
            return init != 0u;
        }

        // The live tree takes a copy of each kept array, or of only that one,
        // so what the game changes in place after (outfits.cpp pushes into
        // characters) is the live tree's alone.
        void syncConfig(const Guest &guest, const Campaign &campaign, std::optional<size_t> only = std::nullopt)
        {
            const uint32_t live = guest.run(s_addresses->systemConfig, {});
            const uint32_t scratch = guest.run(s_addresses->builtinNew, {8u});
            for (size_t i = 0; i < kConfig.size(); ++i)
            {
                if (only && i != *only)
                    continue;
                const uint32_t to = under(guest, live, kConfig[i]);
                if (campaign.config[i] == 0u || to == 0u)
                {
                    std::cerr << "[campaign] no " << kConfig[i].back() << " in the config" << std::endl;
                    continue;
                }
                const uint32_t from = guest.clone(campaign.config[i]);
                sync(guest, to, from, scratch);
                guest.release(from);
            }
            guest.run(s_addresses->builtinDelete, {scratch});
        }

        // The scene a panel made in `init` loads, {new Class name (file
        // x.milo) ...}, as the archive holds it: ui/gen/x.milo_ps2. None for
        // one that works its file out when it loads, or has none.
        std::string sceneOf(const Guest &guest, uint32_t init, const std::string &panel)
        {
            constexpr uint32_t kString = 0x12u;
            for (int i = 0; i < guest.size(init); ++i)
            {
                if (guest.type(init, i) != script::kCommand)
                    continue;
                const uint32_t command = guest.value(init, i);
                if (guest.key(command) != "new" || guest.size(command) < 3 || guest.type(command, 2) != script::kSymbol ||
                    panel != reinterpret_cast<const char *>(getMemPtr(guest.call.rdram, guest.value(command, 2))))
                    continue;
                const uint32_t file = guest.find(command, "file");
                if (file == 0u || guest.size(file) < 2 ||
                    (guest.type(file, 1) != script::kSymbol && guest.type(file, 1) != kString))
                    return "";
                // A string node holds an array whose nodes are its characters.
                const uint32_t text = guest.type(file, 1) == script::kSymbol ? guest.value(file, 1)
                                                                              : load<uint32_t>(guest.call.rdram, guest.value(file, 1));
                const std::string name = reinterpret_cast<const char *>(getMemPtr(guest.call.rdram, text));
                if (name.size() <= 5u || name.compare(name.size() - 5u, 5u, ".milo") != 0)
                    return "";
                return "ui/gen/" + name + "_ps2";
            }
            return "";
        }

        // Each panel `init` makes takes its type definition from there, or
        // only those in `only`. `was` is where a scene came from before.
        using Origin = std::optional<std::pair<uint32_t, uint32_t>>;
        void retypeUi(const Guest &guest, uint32_t init, const std::function<Origin(const std::string &)> &was,
                      const std::set<std::string> *only = nullptr)
        {
            for (int i = 0; i < guest.size(init); ++i)
            {
                if (guest.type(init, i) != script::kCommand)
                    continue;
                const uint32_t command = guest.value(init, i);
                // {foreach $p (pause_panel ... helpbar) {$p load}}: the panels
                // kept loaded from boot. Each whose scene is another file
                // now is let go and loaded again. The help bar's, which is
                // built at boot (ui/help_bar.cpp), is the same one, and stays.
                if (guest.key(command) == "foreach" && guest.size(command) == 4 && guest.type(command, 2) == script::kArray &&
                    guest.type(command, 3) == script::kCommand && guest.size(guest.value(command, 3)) == 2 &&
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
                        if (!scene.empty() && (!only || only->count(name) != 0u) && was(scene) != ark::origin(scene))
                            unload += "{" + name + " unload}\n{" + name + " load}\n";
                    }
                    // Only those: a panel loaded again as it stands is
                    // loaded once more, and one unload later leaves it up.
                    script::run(guest.call.rdram, guest.call.ctx, guest.call.runtime, unload);
                    continue;
                }
                // {new Class name (...)...}, the arrays its type definition.
                if (guest.key(command) != "new" || guest.size(command) < 4 || guest.type(command, 2) != script::kSymbol ||
                    guest.type(command, 3) != script::kArray)
                    continue;
                if (only && only->count(reinterpret_cast<const char *>(getMemPtr(guest.call.rdram, guest.value(command, 2)))) == 0u)
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


        // That campaign's files read, if no switch has read them yet.
        bool built(const Guest &guest, size_t index)
        {
            if (s_campaigns[index].init != 0u)
                return true;
            ark::front(s_campaigns[index].front);
            const bool ok = build(guest, s_campaigns[index]);
            if (ok)
                locale::build(s_campaigns[index].game, guest.call.rdram, guest.call.ctx, guest.call.runtime);
            ark::front(s_campaigns[s_active].front);
            return ok;
        }

        // That campaign's text alone read, if nothing has read it yet: all
        // that its names and blurbs shown outside its career take.
        void textBuilt(const Guest &guest, size_t index)
        {
            ark::front(s_campaigns[index].front);
            locale::build(s_campaigns[index].game, guest.call.rdram, guest.call.ctx, guest.call.runtime);
            ark::front(s_campaigns[s_active].front);
        }

        std::optional<size_t> indexOf(const std::string &game);

        // Outside a career, the game whose screens around a song and highway
        // (gems, strikeline and HUD, all but its surface, which is the
        // guitarist's) it has (in_song_ui): the venue's, or one game's where
        // it is there. In a career, the active one's, whatever note engine
        // judges.
        size_t lookFor(uint8_t *rdram, size_t venue)
        {
            static const char *const kGames[] = {"", "gh1", "gh2", "gh80s"};
            const int pick = settings::get(settings::kInSongUi);
            if (inCareer(rdram))
                return s_active;
            if (pick > settings::kInSongVenue && pick <= settings::kInSongGh80s)
                if (const auto index = indexOf(kGames[pick]))
                    return *index;
            return venue;
        }

        // The highway's files lent from the look's game, the active one's own
        // back when it is that. False if that game cannot be read.
        bool highwayFrom(const script::Call &call, size_t from)
        {
            if (from == s_gemsFrom)
                return true;
            if (!built(Guest{call}, from))
                return false;
            for (const char *prefix : kHighway)
                if (from == s_active)
                    ark::unlend(prefix);
                else
                    ark::lend(prefix, s_campaigns[from].front);
            s_gemsFrom = from;
            s_gh1Highway = s_campaigns[from].game == "gh1";
            std::cerr << "[campaign] highway from " << s_campaigns[from].game << std::endl;
            return true;
        }

        // The next song's venue is that campaign's: its world and venue
        // files from its layer, and its venues' scripts in the config's
        // types. Its screens around it and loading tips are the look's
        // (lookFor), and its highway the same game's. The active one's puts the
        // active one's back.
        bool venuesFrom(const script::Call &call, size_t index)
        {
            const size_t look = lookFor(call.rdram, index);
            // The venue's name is its game's, wherever it shows.
            locale::own(locale::kVenue, s_campaigns[index].game);
            if (index == s_venueFrom && look == s_lookFrom)
                return highwayFrom(call, look);
            const Guest guest{call};
            if (!built(guest, index) || !built(guest, look))
                return false;
            const auto lendFrom = [](const std::string &prefix, size_t from)
            {
                if (from == s_active)
                    ark::unlend(prefix);
                else
                    ark::lend(prefix, s_campaigns[from].front);
            };
            if (index != s_venueFrom)
            {
                syncConfig(guest, s_campaigns[index], kWorldTypes);
                for (const char *prefix : kVenueFiles)
                    lendFrom(prefix, index);
                s_venueFrom = index;
            }
            if (look != s_lookFrom)
            {
                syncConfig(guest, s_campaigns[look], kTips);
                // The song's panels as that game's scripts make them; one kept
                // loaded from boot whose scene is another file now loads again.
                const uint32_t init = guest.clone(s_campaigns[look].init);
                std::map<std::string, Origin> was;
                for (const std::string &panel : kSongPanels)
                    if (const std::string scene = sceneOf(guest, init, panel); !scene.empty())
                        was[scene] = ark::origin(scene);
                for (const std::string &prefix : s_lookLent)
                    ark::unlend(prefix);
                s_lookLent.clear();
                if (look != s_active)
                {
                    for (const auto &[scene, origin] : was)
                        s_lookLent.push_back(scene);
                    s_lookLent.insert(s_lookLent.end(), kSongFiles.begin(), kSongFiles.end());
                    for (const std::string &prefix : s_lookLent)
                        ark::lend(prefix, s_campaigns[look].front);
                }
                retypeUi(guest, init, [&](const std::string &scene) { return was[scene]; }, &kSongPanels);
                guest.release(init);
                // Ours over that game's: the pause menu's rows are the same
                // whichever game's it is.
                script::patchSongUiAgain(call.rdram, call.ctx, call.runtime);
                s_lookFrom = look;
            }
            std::cerr << "[campaign] venues from " << s_campaigns[index].game << ", screens around the song from "
                      << s_campaigns[look].game << std::endl;
            return highwayFrom(call, look);
        }

        // That campaign's menu loops, as its (synth metamusic music) names
        // them, off its files.
        const std::vector<std::string> &musicOf(size_t index)
        {
            static std::map<size_t, std::vector<std::string>> s_found;
            if (const auto it = s_found.find(index); it != s_found.end())
                return it->second;
            const auto front = s_campaigns[index].front;
            dtb::Macros macros;
            const auto synth =
                dtb::read("config/synth.dta", macros, [front](const std::string &path) { return ark::readFront(front, path); });
            const dtb::Node *metamusic = synth ? dtb::find(*synth, "metamusic") : nullptr;
            const dtb::Node *music = metamusic ? dtb::find(*metamusic, "music") : nullptr;
            std::vector<std::string> &loops = s_found[index];
            for (size_t i = 1u; music && i < music->nodes.size(); ++i)
                loops.push_back(music->nodes[i].text);
            return loops;
        }

        // The menus' music outside a career (menu_music): the menus' game's,
        // one game's, or every game's loops, the menus' game's first and all
        // streamed, as GH1's are. In a career, the career's own.
        void musicFrom(const script::Call &call)
        {
            static const char *const kGames[] = {"", "gh1", "gh2", "gh80s"};
            const Guest guest{call};
            const int pick = settings::get(settings::kMenuMusic);
            size_t from = s_menus;
            if (!s_career && pick > settings::kMusicTheme && pick < settings::kMusicAll)
                if (const auto index = indexOf(kGames[pick]))
                    from = *index;
            if (!built(guest, from))
                return;
            syncConfig(guest, s_campaigns[from], kMetaMusic);
            if (s_career || pick != settings::kMusicAll)
                return;
            // The 360's are GH2's, each loop once.
            std::set<std::string> listed(musicOf(from).begin(), musicOf(from).end());
            std::string text = "{do ($m {find $syscfg synth metamusic})";
            for (size_t i = 0; i < s_campaigns.size(); ++i)
                for (const std::string &loop : musicOf(i))
                    if (listed.insert(loop).second)
                        text += " {push_back {find $m music} " + loop + "}";
            text += " {set_elem {find $m play_from_memory} 1 0}}";
            script::run(call.rdram, call.ctx, call.runtime, text);
        }

        // The menus from here on are that campaign's: its scripts' for the
        // menus' panels and screens (kMenuPanels), their scenes and the
        // files beside them, and its menu music. The active one's take back
        // their own. Our scripts' patches go over them after (patchUiAgain).
        bool menusFrom(const script::Call &call, size_t index)
        {
            const Guest guest{call};
            if (!built(guest, index))
                return false;
            const uint32_t init = guest.clone(s_campaigns[index].init);
            std::map<std::string, Origin> was;
            for (const std::string &panel : kMenuPanels)
                if (const std::string scene = sceneOf(guest, init, panel); !scene.empty())
                    was[scene] = ark::origin(scene);
            for (const std::string &prefix : s_menusLent)
                ark::unlend(prefix);
            s_menusLent.clear();
            if (index != s_active)
            {
                for (const auto &[scene, origin] : was)
                    s_menusLent.push_back(scene);
                s_menusLent.insert(s_menusLent.end(), kMenuFiles.begin(), kMenuFiles.end());
                for (const std::string &prefix : s_menusLent)
                    ark::lend(prefix, s_campaigns[index].front);
            }
            retypeUi(guest, init, [&](const std::string &scene) { return was[scene]; }, &kMenuPanels);
            guest.release(init);
            s_menus = index;
            musicFrom(call);
            std::cerr << "[campaign] menus from " << s_campaigns[index].game << ", " << was.size() << " scenes" << std::endl;
            return true;
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
            // Its files read the first time it is switched to, before
            // anything changes.
            ark::front(s_campaigns[index].front);
            if (s_campaigns[index].init == 0u && !build(guest, s_campaigns[index]))
            {
                ark::front(s_campaigns[before].front);
                return false;
            }
            lap("read");
            // The career as it stands goes into the save's sections first.
            ark::front(s_campaigns[before].front);
            save::leaveGame(call.rdram, call.ctx, call.runtime);
            lap("leave");
            ark::front(s_campaigns[index].front);
            s_active = index;
            // The sync brings every array back, the venues' types too, and
            // the retype the song's panels and the menus.
            for (const char *prefix : kVenueFiles)
                ark::unlend(prefix);
            for (const std::string &prefix : s_lookLent)
                ark::unlend(prefix);
            for (const char *prefix : kHighway)
                ark::unlend(prefix);
            s_lookLent.clear();
            s_venueFrom = s_lookFrom = s_gemsFrom = index;
            s_gh1Highway = s_campaigns[index].game == "gh1";
            for (const std::string &prefix : s_menusLent)
                ark::unlend(prefix);
            s_menusLent.clear();
            s_menus = index;
            outfits::campaignChanged();
            syncConfig(guest, s_campaigns[index]);
            // Outside a career the guitars have every game's finishes from
            // here, not from the first picker: a guitar picked before the
            // switch is set again in one of them (ghc_qp_recall).
            if (!s_career)
                script::run(call.rdram, call.ctx, call.runtime, lendFinishes());
            // GameConfig's list of the config's venues, which its constructor
            // (0x125c98) builds once: the career's map opens on the last
            // venue unlocked before the first in it that is not
            // (GameConfig::SetCareerVenue, 0x126c50).
            guest.run(s_addresses->venueProviderInitData,
                      {load<uint32_t>(call.rdram, s_addresses->theGameConfig) + kVenueProvider, 0u});
            lap("sync");
            // The objects take a copy's arrays, as each would a file read
            // again: a property with no value set is its type definition's
            // own node (Hmx::Object::Property, 0x2c1488), which scripts
            // change in place.
            const uint32_t init = guest.clone(s_campaigns[index].init);
            retypeUi(guest, init,
                     [&](const std::string &scene)
                     {
                         ark::front(s_campaigns[before].front);
                         const Origin origin = ark::origin(scene);
                         ark::front(s_campaigns[index].front);
                         return origin;
                     });
            guest.release(init);
            lap("retype");
            script::patchUiAgain(call.rdram, call.ctx, call.runtime);
            lap("patches");
            locale::enter(s_campaigns[index].game, call.rdram, call.ctx, call.runtime);
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

        // Whether going to that campaign's career, or to its menus outside
        // one, has anything to change.
        bool changes(size_t index, bool career)
        {
            return career ? index != s_active || s_menus != index : s_active != 0u || s_menus != index;
        }

        // Into that campaign's career, everything its own; or out of one, or
        // a change of menus outside one: GH2's with that one's menus. Either
        // way that game is the theme from then on, kept for the next boot.
        bool goTo(const script::Call &call, size_t index, bool career)
        {
            // Under the engine picked, in a career or out of one, before any
            // game changes.
            judgeNoteEngine(pickedNoteEngine(), call.rdram, call.ctx, call.runtime);
            s_career = career;
            s_theme = index;
            if (career ? index != s_active : s_active != 0u)
                if (!switchTo(call, career ? index : 0u))
                    return false;
            if (s_menus != index)
            {
                if (!menusFrom(call, index))
                    return false;
                script::patchUiAgain(call.rdram, call.ctx, call.runtime);
            }
            else
                musicFrom(call);
            save::keepLastGame(s_campaigns[index].game);
            return true;
        }

        // The first {game set_character <outfit> ...} under `node`, or for
        // the second player {$variable set_character <outfit> ...}: the
        // players' defaults, which main.dta's main_panel sets.
        const dtb::Node *setCharacter(const dtb::Node &node, bool second)
        {
            if (node.nodes.size() > 2u && node.nodes[1].text == "set_character" && node.nodes[2].type == dtb::kSymbol &&
                (second ? node.nodes[0].type == dtb::kVar : node.nodes[0].text == "game"))
                return &node.nodes[2];
            for (const dtb::Node &child : node.nodes)
                if (const dtb::Node *found = setCharacter(child, second))
                    return found;
            return nullptr;
        }

        // The active game's own, read off its disc: punk1 and rockabill1 in
        // GH2, goth2 the second in the 80s.
        std::string defaultCharacter(bool second)
        {
            static std::map<std::pair<size_t, bool>, std::string> s_found;
            if (const auto it = s_found.find({s_active, second}); it != s_found.end())
                return it->second;
            const auto front = s_campaigns[s_active].front;
            dtb::Macros macros;
            const auto main = dtb::read("ui/main.dta", macros, [front](const std::string &path) { return ark::readFront(front, path); });
            const dtb::Node *outfit = main ? setCharacter(*main, second) : nullptr;
            return s_found[{s_active, second}] = outfit ? outfit->text : "punk1";
        }

        // How far band `slot` is through `game`'s career, 0 to 100: each
        // difficulty with a career song passed a quarter over the one below,
        // by its career songs passed, so expert done is 100 whatever was
        // played under it; the best difficulty's.
        int progress(const std::string &game, int slot, std::optional<NoteEngine> engine)
        {
            const std::vector<std::string> career = setlists::careerSongs(game);
            if (career.empty())
                return 0;
            const auto passed = save::passedSongs(game, slot, engine);
            size_t best = 0u;
            for (size_t d = 0; d < passed.size(); ++d)
            {
                size_t count = 0u;
                for (const std::string &song : career)
                    count += passed[d].count(song);
                if (count > 0u)
                    best = std::max(best, 25u * d + 25u * count / career.size());
            }
            return static_cast<int>(best);
        }

        // Its expert full combos (save::fullComboSongs) as a share of every
        // song, career and bonus, 0 to 100.
        int fullCombos(const std::string &game, int slot, std::optional<NoteEngine> engine)
        {
            const std::set<std::string> combos = save::fullComboSongs(game, slot, engine).back();
            size_t count = 0u, all = 0u;
            for (const std::vector<std::string> &songs : {setlists::careerSongs(game), setlists::bonusSongs(game)})
                for (const std::string &song : songs)
                {
                    count += combos.count(song);
                    ++all;
                }
            return all == 0u ? 0 : static_cast<int>(count * 100u / all);
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

        // What `game`'s config has to use, read off its disc: its characters
        // and their outfits, its guitars and their finishes.
        struct Guitar
        {
            // (as <guitar>) names the game disc's guitar this one is a model
            // of, where the two are named apart (gh1/career.cpp).
            std::string key, type, as;
            std::vector<std::array<std::string, 3>> skins; // key, outfit, mat
        };
        struct Usable
        {
            std::set<std::string> items;
            std::vector<std::string> outfits; // in config's order
            std::vector<std::string> characters;
            std::vector<Guitar> guitars;
        };
        const Usable &usable(const Campaign &campaign)
        {
            static std::map<std::string, Usable> s_found;
            if (const auto it = s_found.find(campaign.game); it != s_found.end())
                return it->second;
            Usable &found = s_found[campaign.game];
            const auto front = campaign.front;
            dtb::Macros macros;
            const auto config =
                dtb::read(kRootConfig, macros, [front](const std::string &path) { return ark::readFront(front, path); });
            const auto keys = [](const dtb::Node &list)
            {
                std::vector<std::string> out;
                for (size_t i = 1u; i < list.nodes.size(); ++i)
                    if (list.nodes[i].type == dtb::kArray && !list.nodes[i].nodes.empty())
                        out.push_back(list.nodes[i].nodes[0].text);
                return out;
            };
            if (const dtb::Node *characters = config ? dtb::find(*config, "characters") : nullptr)
                for (size_t i = 1u; i < characters->nodes.size(); ++i)
                {
                    const dtb::Node &character = characters->nodes[i];
                    if (character.type != dtb::kArray || character.nodes.empty())
                        continue;
                    found.items.insert(character.nodes[0].text);
                    found.characters.push_back(character.nodes[0].text);
                    for (const std::string &outfit : keys(character))
                    {
                        found.items.insert(outfit);
                        found.outfits.push_back(outfit);
                    }
                }
            if (const dtb::Node *guitars = config ? dtb::find(*config, "guitars") : nullptr)
                for (size_t i = 1u; i < guitars->nodes.size(); ++i)
                {
                    const dtb::Node &guitar = guitars->nodes[i];
                    if (guitar.type != dtb::kArray || guitar.nodes.empty())
                        continue;
                    found.items.insert(guitar.nodes[0].text);
                    const auto word = [&](const char *key)
                    {
                        const dtb::Node *node = dtb::find(guitar, key);
                        return node && node->nodes.size() > 1u ? node->nodes[1].text : std::string();
                    };
                    Guitar &kept = found.guitars.emplace_back(Guitar{guitar.nodes[0].text, word("type"), word("as"), {}});
                    const dtb::Node *skins = dtb::find(guitar, "skins");
                    for (size_t s = 1u; skins && s < skins->nodes.size(); ++s)
                    {
                        const dtb::Node &skin = skins->nodes[s];
                        const dtb::Node *outfit = dtb::find(skin, "outfit"), *mat = dtb::find(skin, "mat");
                        if (skin.type != dtb::kArray || !outfit || !mat || outfit->nodes.size() < 2u || mat->nodes.size() < 2u)
                            continue;
                        found.items.insert(skin.nodes[0].text);
                        kept.skins.push_back({skin.nodes[0].text, outfit->nodes[1].text, mat->nodes[1].text});
                    }
                }
            return found;
        }

        // That campaign's venues, in its config's order.
        const std::vector<std::string> &venuesOf(size_t index)
        {
            static std::map<size_t, std::vector<std::string>> s_found;
            if (const auto it = s_found.find(index); it != s_found.end())
                return it->second;
            const auto front = s_campaigns[index].front;
            dtb::Macros macros;
            const auto config =
                dtb::read(kRootConfig, macros, [front](const std::string &path) { return ark::readFront(front, path); });
            const dtb::Node *list = config ? dtb::find(*config, "venues") : nullptr;
            std::vector<std::string> &venues = s_found[index];
            for (size_t i = 1u; list && i < list->nodes.size(); ++i)
                venues.push_back(list->nodes[i].text);
            return venues;
        }

        // Whether a song may play in another campaign's venue outside its
        // career: the first is always open, the rest once a band has
        // unlocked them there.
        bool venueOpen(size_t index, const std::string &venue)
        {
            const std::vector<std::string> &venues = venuesOf(index);
            return (!venues.empty() && venues.front() == venue) || save::unlockedItem(s_campaigns[index].game, venue);
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
                return {script::symbol(call.rdram, call.ctx, call.runtime, s_campaigns[s_menus].game), script::kSymbol};
            if (op == "live")
                return {script::symbol(call.rdram, call.ctx, call.runtime, active()), script::kSymbol};
            if (op == "has")
                return {indexOf(call.symbol(2)) ? 1u : 0u, script::kInt};
            if (op == "first_character" || op == "second_character")
                return {script::symbol(call.rdram, call.ctx, call.runtime, defaultCharacter(op == "second_character")),
                        script::kSymbol};
            if (op == "progress")
            {
                // The career being played as it stands, not as last saved.
                if (call.symbol(2) == active())
                    save::leaveGame(call.rdram, call.ctx, call.runtime);
                return {static_cast<uint32_t>(progress(call.symbol(2), static_cast<int>(call.number(3)),
                                                       noteEngineNamed(call.size() > 4 ? call.symbol(4) : ""))),
                        script::kInt};
            }
            if (op == "full_combos")
            {
                if (call.symbol(2) == active())
                    save::leaveGame(call.rdram, call.ctx, call.runtime);
                return {static_cast<uint32_t>(fullCombos(call.symbol(2), static_cast<int>(call.number(3)),
                                                         noteEngineNamed(call.size() > 4 ? call.symbol(4) : ""))),
                        script::kInt};
            }
            // From onFinishSong: band, difficulty, song, then player 0's
            // longest streak, gems hit, gems passed and percent hit, which
            // is of the song's every gem, so a song cut short is none.
            if (op == "finished")
            {
                const auto longest = call.number(5), hit = call.number(6), passed = call.number(7);
                if (hit > 0 && passed == 0 && longest == hit && call.number(8) == 100)
                    save::keepFullCombo(static_cast<int>(call.number(2)), static_cast<int>(call.number(3)),
                                        call.symbol(4));
                return {};
            }
            if (op == "count")
                return {static_cast<uint32_t>(s_campaigns.size()), script::kInt};
            if (op == "venues")
            {
                const auto index = indexOf(call.symbol(2));
                std::string names;
                for (const std::string &venue : index ? venuesOf(*index) : std::vector<std::string>())
                    names += venue + " ";
                return {script::parse(call.rdram, call.ctx, call.runtime, names), script::kArray};
            }
            if (op == "venue_open")
            {
                const auto index = indexOf(call.symbol(2));
                return {index && venueOpen(*index, call.symbol(3)) ? 1u : 0u, script::kInt};
            }
            if (op == "venue_game")
                return {script::symbol(call.rdram, call.ctx, call.runtime, venueGame()), script::kSymbol};
            if (op == "highway")
                return {script::symbol(call.rdram, call.ctx, call.runtime, s_campaigns[s_gemsFrom].game), script::kSymbol};
            if (op == "read_text")
            {
                for (size_t i = 0; i < s_campaigns.size(); ++i)
                    textBuilt(Guest{call}, i);
                return {};
            }
            if (op == "venues_from")
            {
                const auto index = indexOf(call.symbol(2));
                return {index && venuesFrom(call, *index) ? 1u : 0u, script::kInt};
            }
            // Whose names the venues show from here on (a map's), and whose
            // a character's name and blurbs are: the outfit's game's.
            if (op == "text_venues" || op == "text_outfit")
            {
                const auto own = op == "text_venues" ? std::optional<std::pair<std::string, std::string>>()
                                                     : outfits::ownOf(call.symbol(2));
                const auto index = indexOf(op == "text_venues" ? call.symbol(2) : own ? own->first : active());
                if (!index)
                    return {0u, script::kInt};
                textBuilt(Guest{call}, *index);
                locale::own(op == "text_venues" ? locale::kVenue : locale::kCharacter, s_campaigns[*index].game);
                return {1u, script::kInt};
            }
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
                const bool career = call.symbol(4) == "career";
                if (!index)
                    return {0u, script::kInt};
                if (!changes(*index, career) && pickedNoteEngine() == noteEngine())
                {
                    s_career = career;
                    s_theme = *index;
                    save::keepLastGame(s_campaigns[*index].game);
                    return {0u, script::kInt};
                }
                s_pending = Pending{*index, call.size() > 3 ? call.symbol(3) : "", career};
                return {1u, script::kInt};
            }
            std::cerr << "[campaign] usage: {campaigns active|live|count}, {campaigns has <game>}, "
                         "{campaigns switch <game> [<screen> [career|menus]]}, "
                         "{campaigns progress <game> <band> [<engine>]}, {campaigns full_combos <game> <band> [<engine>]}"
                      << std::endl;
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
            // The game's code here runs some three times faster than its
            // cycles count for, time the game would then wait out.
            const EeScheduler::UnpacedScope unpaced(runtime->eeScheduler());
            script::run(rdram, ctx, runtime, "{set $ghc_band {campaign profile_slot}}");
            if (!goTo(script::Call{rdram, ctx, runtime, 0u}, pending.index, pending.career))
                script::run(rdram, ctx, runtime, "{ui goto_screen main_screen}");
            else
                script::run(rdram, ctx, runtime,
                            "{campaign set_profile_slot $ghc_band}" +
                                (pending.next.empty() ? std::string() : "{ui goto_screen " + pending.next + "}"));
            // The next screen's scenes load here and now, not at LoadMgr's
            // 10 ms a frame (its period, +0x24): LoadMgr::Poll (0x2cc218)
            // runs its loaders until none is left or that is spent, as
            // PollUntilLoaded (0x2cc0f8) has it do.
            const uint32_t loads = s_addresses->theLoadMgr;
            const float period = load<float>(rdram, loads + 0x24u);
            store<float>(rdram, loads + 0x24u, 1e30f);
            runtime->callGuestFunction(rdram, ctx, s_addresses->loadMgrPoll, {loads});
            store<float>(rdram, loads + 0x24u, period);
            *ctx = saved;
        }
    }

    namespace
    {
        // The boot's first themed screen, once the card check has loaded the
        // save (LoadData1, save.cpp): the intro movie's (splash.dta's
        // cut_scene_screen, which every card check goes on to, and
        // fast_boot.dta's too), else the main menu's. Where another game was
        // the last entered (save::lastGame), by way of the screen the switch
        // happens on (campaigns.dta), so the movie, the splash and the menus
        // are all that one's.
        // Whose text a screen's own tokens take (content/locale.h): the
        // menus' game's on the menus, the in-song look's on the screens
        // around a song, else the active one's. The venues' names and the
        // characters' are owned apart, by the venue and the outfit.
        void ownScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t screen)
        {
            static std::unordered_map<uint32_t, bool> s_menuScreens; // each menu's or song's screen, true for a menu's
            if (s_menuScreens.empty())
            {
                const R5900Context saved = *ctx;
                std::vector<std::pair<std::string, bool>> names;
                for (const auto *set : {&kMenuPanels, &kSongPanels})
                    for (const std::string &name : *set)
                        if (name.size() > 7u && name.compare(name.size() - 7u, 7u, "_screen") == 0)
                            names.emplace_back(name, set == &kMenuPanels);
                // The song's own, whose HUD is the look's too.
                names.emplace_back("game_screen", false);
                names.emplace_back("practice_game_screen", false);
                std::string text;
                for (const auto &[name, menu] : names)
                    text += name + " ";
                const uint32_t parsed = script::parse(rdram, ctx, runtime, text);
                for (size_t i = 0; parsed != 0u && i < names.size(); ++i)
                    if (const uint32_t object = static_cast<uint32_t>(runtime->callGuestFunction(
                            rdram, ctx, s_addresses->dataNodeGetObj,
                            {load<uint32_t>(rdram, parsed) + 8u * static_cast<uint32_t>(i), parsed})))
                        s_menuScreens.emplace(object, names[i].second);
                // The venues' names and the characters', with their blurbs.
                std::set<std::string> venues, characters;
                for (size_t i = 0; i < s_campaigns.size(); ++i)
                    for (const std::string &venue : venuesOf(i))
                        venues.insert(venue);
                for (const std::string &character : usable(s_campaigns[0]).characters)
                    for (const char *suffix : {"", "_blurb", "_outfit_blurb"})
                        characters.insert(character + suffix);
                locale::ownTokens(locale::kVenue, venues);
                locale::ownTokens(locale::kCharacter, characters);
                *ctx = saved;
            }
            const auto it = s_menuScreens.find(screen);
            const size_t owner = it == s_menuScreens.end() ? s_active : it->second ? s_menus : s_lookFrom;
            locale::own(locale::kScreen, s_campaigns[owner].game);
        }

        // A career song passed, alone (ProfileState::FinishSong, from
        // {campaign finish_song}; co-op has its own): kept as a full combo
        // where player 0 hit every gem and its longest streak is all of
        // them ({campaigns finished}).
        struct FinishSongTag;
        void onFinishSong(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime,
                        "{campaigns finished {campaign profile_slot} {game get_difficulty 0} {game get_song} "
                        "{player0 longest_streak} {player0 gems_hit} {player0 gems_passed} {player0 percent_hit}}");
            *ctx = saved;
        }

        struct GotoScreenTag;
        void onGotoScreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static bool s_opened = false;
            static uint32_t screens[3] = {0u, 0u, 0u}; // cut_scene_screen, main_screen, ghc_switch_screen
            static const char *const kOpening[] = {"cut_scene_screen", "main_screen"};
            const uint32_t screen = GPR_U32(ctx, 5);
            s_toSwitch = screen != 0u && screen == screens[2];
            ownScreen(rdram, ctx, runtime, screen);
            // Out of a career to the main menu, by way of the switch to the
            // menus picked for outside one.
            if (s_opened && s_career && screen != 0u && screen == screens[1])
            {
                const R5900Context saved = *ctx;
                script::run(rdram, ctx, runtime,
                            "{set $ghc_switch " + s_campaigns[s_theme].game +
                                "} {set $ghc_switch_next main_screen} {set $ghc_switch_as menus}");
                *ctx = saved;
                SET_GPR_U32(ctx, 5, screens[2]);
                s_toSwitch = true;
                return;
            }
            if (s_opened || screen == 0u)
                return;
            const R5900Context saved = *ctx;
            // The UI scripts, which make the switch's screen, run at the first
            // screen change after the commands register (script.h), in a hook
            // that runs after this one: the intro movie's, on a boot.
            script::readyUi(rdram, ctx, runtime);
            const uint32_t names = script::parse(rdram, ctx, runtime, "cut_scene_screen main_screen ghc_switch_screen");
            for (uint32_t i = 0u; names != 0u && i < 3u; ++i)
                screens[i] = static_cast<uint32_t>(runtime->callGuestFunction(
                    rdram, ctx, s_addresses->dataNodeGetObj, {load<uint32_t>(rdram, names) + 8u * i, names}));
            const int opening = screens[0] != 0u && screens[0] == screen   ? 0
                                : screens[1] != 0u && screens[1] == screen ? 1
                                                                           : -1;
            const std::string last = opening >= 0 ? save::lastGame() : "";
            const bool switching = opening >= 0 && screens[2] != 0u && indexOf(last) && last != active();
            if (switching)
                script::run(rdram, ctx, runtime,
                            "{set $ghc_switch " + last + "} {set $ghc_switch_next " + kOpening[opening] +
                                "} {set $ghc_switch_as menus}");
            // With no switch, the menu music picked is set here.
            else if (opening >= 0 && settings::get(settings::kMenuMusic) != settings::kMusicTheme)
                musicFrom(script::Call{rdram, ctx, runtime, 0u});
            *ctx = saved;
            if (opening < 0)
                return;
            s_opened = true;
            // The switch finds the boot's scenes again as the game disc has
            // them, and so loads its own over them (retypeUi).
            ark::bootFrom(std::nullopt, {});
            if (switching)
            {
                SET_GPR_U32(ctx, 5, screens[2]);
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
        ark::alias("ghc/" + game + "/", front);
    }

    void over(size_t layer)
    {
        s_campaigns[0].front = layer;
        if (s_active == 0u)
            ark::front(layer);
    }

    const std::string &active()
    {
        return s_campaigns[s_active].game;
    }

    bool career()
    {
        return s_career;
    }

    std::vector<std::string> games()
    {
        std::vector<std::string> out;
        for (const Campaign &campaign : s_campaigns)
            out.push_back(campaign.game);
        return out;
    }

    const std::vector<std::string> &venues(const std::string &game)
    {
        static const std::vector<std::string> s_none;
        const auto index = indexOf(game);
        return index ? venuesOf(*index) : s_none;
    }

    const std::string &venueGame()
    {
        return s_campaigns[s_venueFrom].game;
    }

    bool unlockedIn(const std::string &game, const std::string &item)
    {
        const auto index = indexOf(game);
        if (!index || usable(s_campaigns[*index]).items.count(item) == 0u)
            return false;
        if (sells(s_campaigns[*index], item))
            return save::unlockedItem(game, item);
        // What no store sells is a band's from the start, but for a finish
        // of a guitar that is not, the guitar named battleaxe, which the
        // game keeps locked until it is handed over
        // (ProfileState::InitGuitars, 0x13a888), and the Grim Ripper's own,
        // which comes with him.
        for (const Guitar &guitar : usable(s_campaigns[*index]).guitars)
        {
            if (guitar.key == item)
                return item == "battleaxe" ? save::unlockedItem(game, item) : guitar.type != "grim" || unlockedIn(game, "grim");
            for (const auto &skin : guitar.skins)
                if (skin[0] == item)
                    return unlockedIn(game, guitar.key);
        }
        return true;
    }

    std::optional<bool> guitarUnlocked(const std::string &item)
    {
        bool known = false;
        for (const Campaign &campaign : s_campaigns)
            for (const Guitar &guitar : usable(campaign).guitars)
            {
                const bool finish = std::any_of(guitar.skins.begin(), guitar.skins.end(),
                                                [&](const auto &skin) { return skin[0] == item; });
                if (!finish && (guitar.as.empty() ? guitar.key : guitar.as) != item)
                    continue;
                known = true;
                if (unlockedIn(campaign.game, finish ? item : guitar.key))
                    return true;
            }
        return known ? std::optional<bool>(false) : std::nullopt;
    }

    bool lentFinish(const std::string &item)
    {
        if (usable(s_campaigns[s_active]).items.count(item) != 0u)
            return false;
        for (const Campaign &campaign : s_campaigns)
            for (const Guitar &guitar : usable(campaign).guitars)
                for (const auto &skin : guitar.skins)
                    if (skin[0] == item)
                        return true;
        return false;
    }

    const std::string &lendFinishes()
    {
        static std::map<size_t, std::string> s_text;
        const auto made = s_text.find(s_active);
        if (made != s_text.end())
            return made->second;
        std::string &text = s_text[s_active];
        for (size_t i = 0; i < s_campaigns.size(); ++i)
            for (const Guitar &guitar : i == s_active ? std::vector<Guitar>() : usable(s_campaigns[i]).guitars)
            {
                const std::string body = guitar.as.empty() ? guitar.key : guitar.as;
                // A guitar the live config has none of comes whole. No band
                // has an item for it (ProfileState::InitGuitars ran on the
                // config before this), so no career's picker lists it.
                if (guitar.type == "guitar")
                    text += "{if {! {find_exists $syscfg guitars " + body + "}} {push_back {find $syscfg guitars} (" + body +
                            " (type guitar) (skins))}}\n";
                for (const auto &skin : guitar.skins)
                {
                    const std::string skins = "$syscfg guitars " + body + " skins";
                    text += "{if {&& {find_exists " + skins + "} {! {find_exists " + skins + " " + skin[0] + "}}} {push_back {find " +
                            skins + "} (" + skin[0] + " (outfit " + skin[1] + ") (mat " + skin[2] + "))}}\n";
                }
            }
        return text;
    }

    std::string ownFinishes(uint8_t *rdram)
    {
        std::set<std::string> names;
        for (const Campaign &campaign : s_campaigns)
            for (const Guitar &guitar : usable(campaign).guitars)
                names.insert(guitar.as.empty() ? guitar.key : guitar.as);
        const uint32_t held = load<uint32_t>(rdram, s_addresses->theCampaign);
        const bool career = inCareer(rdram), all = held != 0u && load<uint32_t>(rdram, held + kAllAccess) != 0u;
        // Each such finish by its guitar and key: what it is, and whether
        // the config is to have it.
        std::map<std::pair<std::string, std::string>, std::pair<std::string, bool>> finishes;
        for (size_t i = 0; i < s_campaigns.size(); ++i)
            for (const Guitar &guitar : usable(s_campaigns[i]).guitars)
                for (const auto &skin : guitar.skins)
                    if (names.count(skin[0]) != 0u)
                    {
                        auto &finish = finishes[{guitar.as.empty() ? guitar.key : guitar.as, skin[0]}];
                        finish.first = "(" + skin[0] + " (outfit " + skin[1] + ") (mat " + skin[2] + "))";
                        finish.second = finish.second ||
                                        (career ? i == s_active : all || unlockedIn(s_campaigns[i].game, guitar.key));
                    }
        std::string text;
        for (const auto &[at, finish] : finishes)
        {
            const std::string skins = "$syscfg guitars " + at.first + " skins";
            text += finish.second ? "{if {&& {find_exists " + skins + "} {! {find_exists " + skins + " " + at.second +
                                        "}}} {push_back {find " + skins + "} " + finish.first + "}}\n"
                                  : "{if {find_exists " + skins + " " + at.second + "} {remove_elem {find " + skins + "} {find " +
                                        skins + " " + at.second + "}}}\n";
        }
        return text;
    }

    std::vector<std::string> outfits(const std::string &game)
    {
        const auto index = indexOf(game);
        return index ? usable(s_campaigns[*index]).outfits : std::vector<std::string>();
    }

    bool gh1Highway()
    {
        return s_gh1Highway;
    }

    bool inCareer(uint8_t *rdram)
    {
        // GameConfig's mode Symbol, +0x3c (SongProvider::GetHighScore, 0x117b38).
        const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
        const uint32_t mode = config ? load<uint32_t>(rdram, config + 0x3cu) : 0u;
        return mode != 0u && std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, mode)), "career") == 0;
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        ark::alias("ghc/gh2/", std::nullopt);
        // The boot's logos (App::App's Splash::Show, 0x2139f0) and the card
        // check's poster, the dialog panel loaded with the UI, come before the
        // save that the switch to the last game waits for: they are that
        // game's from the start.
        if (const auto last = indexOf(save::lastGame()); last && *last != s_active)
            ark::bootFrom(s_campaigns[*last].front,
                          {"ui/gen/harmonix_splash.milo_ps2", "ui/gen/activision_splash.milo_ps2",
                           "ui/gen/pub_splash.milo_ps2", "ui/gen/dialog.milo_ps2"});
        script::addCommand("campaigns", campaignsCommand);
        // Every campaign's text read at the boot, where the pickers would
        // stop for one the first time a name of its own is shown.
        script::runWhenUiReady("{campaigns read_text}");
        EntryHook<PollTag>::install(runtime, addresses.uiManagerPoll, onUiPoll);
        EntryHook<GotoScreenTag>::install(runtime, addresses.uiGotoScreen, onGotoScreen);
        EntryHook<FinishSongTag>::install(runtime, addresses.profileStateFinishSong, onFinishSong);
        EntryHook<MetaMusicStopTag>::install(runtime, addresses.metaMusicStop, onMetaMusicStop);
    }
}
