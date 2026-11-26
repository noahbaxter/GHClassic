#pragma once

// A song's best on one note engine, part, difficulty and controller, as each
// profile (save/profiles.h) keeps every song's apart from the careers:
//
//   [profile 1 best]
//   version = 1
//   gh2.shoutatthedevil.gh2.guitar.expert.guitar = 123456 5 98 412 0
//   custom.3f9a12c4d5e6f708.gh1.bass.hard.gamepad = ...
//
// the song (its game's and name, or a custom chart's hash), engine, part,
// difficulty and controller (guitar or gamepad, a keyboard a guitar), and
// the speed where it is not 100% (".s150", save/results.h); then
// the best run's score, stars, percent hit and longest streak, all one run's,
// and whether any run was a full combo (1 or 0). Before profiles every best
// was in [best].

#include <cstdint>
#include <optional>
#include <string>

namespace gh2::save
{
    constexpr const char *kBestSection = "best";
    constexpr int kBestVersion = 1;

    struct Best
    {
        uint32_t score = 0u, stars = 0u, percent = 0u, streak = 0u;
        bool fullCombo = false;
        bool failed = false; // played on past the rock meter running out (no_fail)
    };

    std::optional<Best> parseBest(const std::string &text);
    std::string bestText(const Best &best);
    // The run of the two with the higher score, `a` on a tie, a full combo
    // if either was.
    Best better(const Best &a, const Best &b);

    std::string bestKey(const std::string &song, const std::string &engine, const std::string &part,
                        const std::string &difficulty, const std::string &controller);
    // The section profile `profile` (from 0) keeps its bests in, [best] for
    // none. A multiplayer mode's are apart, each player's own run in it:
    // [profile 1 coop best], [coop best], [profile 2 pro_faceoff best].
    std::string bestSection(std::optional<int> profile, const std::string &mode = "");
}
