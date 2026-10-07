#include "gh1/screens.h"

namespace gh2::gh1
{
    const std::vector<Screen> &screens()
    {
        static const std::vector<Screen> kScreens = {
            {"main"},
            {"chooseprof"},
            {"nameprof"},
            {"sel_diff_career"},
            {"sel_difficulty"},
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
            {"complete"},
        };
        return kScreens;
    }
}
