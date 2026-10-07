#include "gh1/screens.h"

namespace gh2::gh1
{
    namespace
    {
        // Where an object of GH2's goes to be out of sight.
        constexpr float kAway = -5000.0f;

        // GH1's portraits and their lit Views, named for its folders, by
        // GH2's characters (gh1/guitarist.h), for scripts to find by them.
        const std::vector<std::pair<const char *, const char *>> kHeroes = {
            {"sc_char_hair_metal", "sc_char_glam"}, {"sc_char_nu_metal", "sc_char_goth"}, {"sc_char_hiphop", "sc_char_funk1"}};
    }

    const std::vector<Screen> &screens()
    {
        static const std::vector<Screen> kScreens = {
            {"main"},
            {"chooseprof"},
            {"nameprof"},
            {"sel_diff_career"},
            {"sel_difficulty"},
            {"career"},
            // GH2's venue buttons are named for its venues, here those
            // standing in for GH1's (gh1/songs.h), and its map takes its
            // frame by a group of its anims.
            {.gh1 = "sel_venue",
             .renamed = {{"sv_basement.btn", "sv_battle.btn"},
                         {"sv_small_club.btn", "sv_small1.btn"},
                         {"sv_big_club.btn", "sv_big.btn"}},
             .without = {"sv_small2.btn", "sv_stone.btn"},
             .frames = {{"venue_anims.grp", "sv_map.view"}}},
            // GH1 has one setlist for career and quickplay.
            {"sel_song"},
            {"sel_song", "sel_song_quickplay"},
            // GH2's options are six: GH1's game settings are its audio
            // settings and GH1's data settings its memory card, and its
            // video settings and band screens have rows in their look.
            {.gh1 = "options",
             .renamed = {{"op_game.btn", "op_audio.btn"}, {"op_data.btn", "memory_card.btn"}},
             .rows = {{"op_audio.btn", 30.0f},
                      {"video_settings.btn", 2.0f, "op_audio.btn", "op_video_settings"},
                      {"op_data.btn", -26.0f, "memory_card.btn", "op_data_settings"},
                      {"memory_card.btn", -54.0f},
                      {"op_bonus.btn", -82.0f},
                      {"op_credit.btn", -110.0f}}},
            {"tutorials"},
            {"bonus_material"},
            {"loading"},
            {"endgame"},
            {"cashaward"},
            {"complete"},
            // GH1 picks a hero from a wall of portraits, each lit by its own
            // View, for the script to light (ui/dta/sel_character.dta).
            // GH2's list still does the picking, out of sight, and its
            // placer stands the hero where GH1's own scene for one does
            // (char_single.gh).
            {.gh1 = "sel_character",
             .renamed = kHeroes,
             .placed = {{"char_single.placer", -10.0f, -640.0f, -48.0f}, {"character.lst", kAway, 0.0f, 0.0f}}},
            // GH2's guitar is its display's, shown by a proxy, here in GH1's
            // case.
            {.gh1 = "sel_guitar", .without = {"guitar.grp"}, .placed = {{"guitar.pxy", 15.0f, -685.0f, -10.0f, 0.8f}}},
            // GH2's guitar and character stand in GH1's shop window (the
            // character's group is moved by the script, menus.dta), and its
            // song logos, which GH1 has none of, out of sight.
            {.gh1 = "store",
             .with = {"char_placer.grp"},
             .placed = {{"guitar.grp", -9.4f, -743.0f, -1.4f, 0.0f, 0.3f},
                        {"song_logos.pic", kAway, 0.0f, 0.0f},
                        {"st_sold.mesh", kAway, 0.0f, 0.0f}}},
            {"store_back"},
            // GH1 marks a new status with a newspaper, where GH2 has a
            // sponsor's letter for the status reached (endgame.dta's
            // status_panel).
            {"status_1", "sponsorship1"},
            {"status_2", "sponsorship2"},
            {"status_3", "sponsorship3"},
            {"status_4", "sponsorship4"},
            {"status_5", "sponsorship5"},
            {"status_6", "sponsorship6"},
            {"status_7", "sponsorship7"},
            // GH2's pause has a row for each of its settings screens where
            // GH1's has one for its only: five rows in the room of four.
            {.gh1 = "pause",
             .renamed = {{"pause_resume.btn", "resume.btn"},
                         {"pause_restart.btn", "restart.btn"},
                         {"pause_options.btn", "audio_options.btn"},
                         {"pause_quit.btn", "quit.btn"}},
             .rows = {{"resume.btn", 0.0f},
                      {"restart.btn", -14.0f},
                      {"audio_options.btn", -28.0f},
                      {"video_options.btn", -42.0f, "audio_options.btn", "VIDEO"},
                      {"quit.btn", -56.0f}}},
            {.gh1 = "pause_controller", .renamed = {{"pause_controller_resume.btn", "resume.btn"}}},
            // GH1 has one list of buttons where GH2 has one for a career
            // and one for the rest: its own is GH2's for the rest, shown in
            // a career too (menus.dta).
            {.gh1 = "lose",
             .renamed = {{"lose_buttons.view", "normal_buttons.view"},
                         {"lose_restart.btn", "lose_restart_normal.btn"},
                         {"lose_selsong.btn", "lose_selsong_normal.btn"},
                         {"lose_quit.btn", "lose_quit_normal.btn"}}},
        };
        return kScreens;
    }
}
