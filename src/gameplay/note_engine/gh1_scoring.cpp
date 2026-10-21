// GH1's numbers where its scoring.dta differs from GH2's
// (config/gen/scoring.dta on each disc):
//
// - Crowd meter range: the low end is 0.6 on medium and 0.65 on hard,
//   where GH2's are 0.5 and 0.6.
// - Star thresholds (2.0 3.0) on every difficulty: GH2's top one is 2.8.
//
// GH2 reads these from the loaded config each time it needs them
// (Scoring::GetCrowdConfig 0x11c400, GetNumStars 0x11c4c8, config at
// Scoring+0x24). So they are overwritten in memory under GH1's engine, and
// GH2's written back under GH2's.

#include "gameplay/note_engine/gh1.h"

#include "guest/data_array.h"
#include "hook.h"

#include <array>
#include <iostream>
#include <optional>
#include <vector>

namespace gh2::gh1_rules
{
    namespace
    {
        using guest::DataArray;
        using guest::DataNode;

        // One number in scoring.dta: scoring > section > list > difficulty
        // (> field), then the index-th entry there.
        struct ScoringValue
        {
            const char *section, *list;
            int difficulty; // each difficulty's list starts with its number, easy 0 to expert 3
            const char *field; // a named list inside the difficulty's, or null for the difficulty's own
            int index;
            float gh1;
        };
        constexpr ScoringValue kScoring[] = {
            {"default_config", "crowd", 1, "range", 1, 0.6f},
            {"default_config", "crowd", 2, "range", 1, 0.65f},
            {"star_ratings", "thresholds", 0, nullptr, 2, 3.0f},
            {"star_ratings", "thresholds", 1, nullptr, 2, 3.0f},
            {"star_ratings", "thresholds", 2, nullptr, 2, 3.0f},
            {"star_ratings", "thresholds", 3, nullptr, 2, 3.0f},
        };

        const Addresses *s_addresses = nullptr;
        NoteEngine s_scoringEngine = NoteEngine::kGh2; // whose numbers the live config holds
        bool s_scoringMissing = false; // the config lacks one, so it is left alone
        std::array<float, std::size(kScoring)> s_gh2Scoring{}; // as it held them under GH2's

        // Each of kScoring's nodes in the live config, or none if it lacks one.
        std::optional<std::vector<DataNode>> scoringNodes(const DataArray &scoring)
        {
            std::vector<DataNode> nodes;
            for (const ScoringValue &value : kScoring)
            {
                DataArray array = scoring.find(value.section).find(value.list).find(value.difficulty);
                if (value.field)
                    array = array.find(value.field);
                if (array.size() <= value.index)
                {
                    std::cerr << "[note_engine] no scoring " << value.list << " for difficulty " << value.difficulty
                              << " in the config; scoring stays " << noteEngineName(s_scoringEngine) << "'s"
                              << std::endl;
                    return std::nullopt;
                }
                nodes.push_back(array.node(value.index));
            }
            return nodes;
        }

        // CrowdRating::Configure, as each song's players are made: the live
        // config takes the engine's scoring first.
        struct ConfigureTag;
        void onCrowdConfigure(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const NoteEngine engine = noteEngine();
            if (s_scoringMissing || s_scoringEngine == engine)
                return;
            const EntryArgs args(ctx);
            const DataArray config(rdram, static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, s_addresses->systemConfig, {})));
            args.restore(ctx);
            const std::optional<std::vector<DataNode>> nodes = scoringNodes(config.find("scoring"));
            if (!nodes)
            {
                s_scoringMissing = true;
                return;
            }
            // Leaving GH2's: save its numbers before they are overwritten, so
            // switching back can put them back.
            for (size_t i = 0; i < nodes->size(); ++i)
            {
                if (s_scoringEngine == NoteEngine::kGh2)
                    s_gh2Scoring[i] = (*nodes)[i].number();
                (*nodes)[i].setFloat(engine == NoteEngine::kGh1 ? kScoring[i].gh1 : s_gh2Scoring[i]);
            }
            s_scoringEngine = engine;
        }
    }

    void installScoring(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        // Under either engine: GH2's put back.
        EntryHook<ConfigureTag>::install(runtime, addresses.crowdRatingConfigure, onCrowdConfigure);
    }
}
