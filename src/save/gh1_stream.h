#pragma once

// What GH1's Campaign::Save (SLUS-21224 0x138b90) writes, field for field:
// little-endian, a Symbol or String as its length then its characters. Its
// high scores are kept per band: 8 bands by 4 difficulties of 5 entries.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::save::gh1
{
    constexpr int kProfiles = 8;
    constexpr int kDifficulties = 4;
    constexpr int kScoresPerList = 5;
    constexpr int32_t kSongType = 1;
    constexpr size_t kOptionsSize = 19; // OptionData::Save (0x107218), version 1, float volumes
    constexpr uint32_t kDataSize = 0x1e000; // the card file, written whole (SaveData1 0x14a730)

    // CampaignItem::Save (0x154d40): flags bit 0 store, 1 unlocked, 2 passed
    // (songs only); a song adds its score and stars.
    struct ItemState
    {
        uint8_t flags = 0;
        int32_t score = 0;
        uint8_t stars = 0;
    };

    struct Item
    {
        std::string name;
        int32_t type = 0; // 0 venue, 1 song, 2 character, 3 guitar, 4 skin, 5 video
        std::array<ItemState, kProfiles * kDifficulties> states; // band then difficulty
    };

    // CampaignState::Save (0x13f3d8): a difficulty's look, and its cash.
    struct Difficulty
    {
        std::string character, guitar, skin;
        int32_t cash = 0;
    };

    // ProfileState::Save (0x14f920).
    struct Profile
    {
        uint8_t empty = 1;
        std::string name;
        int32_t tutorials = 0;
        int32_t difficulty = 0;
        std::array<Difficulty, kDifficulties> difficulties;
        std::array<uint8_t, kOptionsSize> options{};
    };

    struct ScoreEntry
    {
        std::string name; // char[8], no NUL when full
        int32_t score = 0;
    };

    using ScoreList = std::array<ScoreEntry, kScoresPerList>;

    struct SongScores
    {
        std::string song;
        std::array<ScoreList, kProfiles * kDifficulties> lists; // band * 4 + difficulty
    };

    struct Save
    {
        int32_t version = 6;
        std::array<Profile, kProfiles> profiles;
        std::vector<Item> items;
        int32_t scoreVersion = 1; // HighScoreDB::Save (0x150ec8)
        std::vector<SongScores> scores;
    };

    // A save of no progress: the bands empty as the Campaign constructor
    // (0x1374b0) leaves them, their options at OptionData's defaults, no items
    // (the game keeps its own), and no songs.
    Save blank();

    std::optional<Save> parse(const uint8_t *data, size_t size);
    std::vector<uint8_t> write(const Save &save);
}
