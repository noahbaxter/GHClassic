#pragma once

// What Campaign::Save (retail 0x12ec90) writes, field for field. Integers are
// little-endian; a Symbol or String is its length, then its characters.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gh2::save::gh2
{
    constexpr int kProfiles = 8;
    constexpr int kDifficulties = 4;
    constexpr int kScoresPerList = 5;
    constexpr int32_t kSongType = 1; // CampaignItem::kSong
    constexpr size_t kOptionsSize = 24;

    // CampaignItem::Save (0x140b00): flags bit 0 store, 1 unlocked, 2 passed.
    // Only a song writes the rest: its score, stars (bit 7 gold) and +0xD.
    struct ItemState
    {
        uint8_t flags = 0;
        int32_t score = 0;
        uint8_t stars = 0;
        uint8_t trickle = 0;

        bool operator==(const ItemState &) const = default;
    };

    struct Item
    {
        std::string name;
        int32_t type = 0;
        std::vector<ItemState> states; // per profile, or per profile then difficulty
    };

    // ProfileState::Save (0x13bff8). Each difficulty's look is the
    // character, outfit, guitar and finish (CampaignState::Save, 0x1331f0).
    struct Profile
    {
        uint8_t empty = 1;
        std::string name;
        int32_t tutorials = 0;
        int32_t difficulty = 0;
        int32_t cash = 0;
        std::array<std::array<std::string, 4>, kDifficulties> look;
    };

    struct ScoreEntry
    {
        std::string name; // char[20] in the stream
        int32_t score = 0;

        bool operator==(const ScoreEntry &) const = default;
    };

    struct SongScores
    {
        std::string song;
        std::array<std::array<ScoreEntry, kScoresPerList>, kDifficulties> lists;
    };

    struct Save
    {
        int32_t version = 0;
        std::array<Profile, kProfiles> profiles;
        std::vector<Item> profileItems;  // 8 states each
        std::vector<Item> campaignItems; // 8 x 4 states each
        int32_t scoreVersion = 0;        // HighScoreDB::Save (0x13d1f0)
        std::vector<SongScores> scores;
        std::string defaultName;
        uint8_t careerBeaten = 0; // Campaign +0x78
        // CoopCampaign::Save (0x133d30): songs by position, in InitSongs'
        // (0x133650) order: expert's career songs, then the store's.
        std::vector<ItemState> coop;
        std::array<uint8_t, kOptionsSize> options{}; // OptionData::Save (0x10d330)
    };

    // A save of no progress: the bands empty as Campaign's constructor leaves
    // them, OptionData's defaults (0x10d2e0), no items or co-op songs (the game
    // keeps its own) and no high scores.
    Save blank();

    // Nothing when the bytes are not a whole save: all access on, Campaign::Save
    // writes none.
    std::optional<Save> parse(const uint8_t *data, size_t size);
    std::vector<uint8_t> write(const Save &save);
}
