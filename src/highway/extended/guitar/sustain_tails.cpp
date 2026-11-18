// GH2 draws a Gem's every rep's tail its one length: Gem::Poll gives
// Gem::ApplyDuration (0x157ce0) the tail's start, end and the track's far
// end, along the track from the gem (its place +0x10 to its end +0x14), and
// it passes them to each rep's GemRep::SetDuration. For a split chord each
// rep's end is drawn back to its lane's share of the gem's length, and its
// start no further than that, so a lane done burns no more.

#include "highway/extended/guitar/sustain_tails.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "guest.h"
#include "guest/beatmatch.h"
#include "highway/gem_looks.h"
#include "hook.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <map>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // Each split chord's Gem on the track: its reps' shares of its
        // length, a rep a fret, lowest first (Gem::AddRep, 0x157960).
        std::map<uint32_t, std::vector<float>> s_shares;

        // Gem: its place along the track and its end.
        constexpr uint32_t kGemFrom = 0x10u, kGemTo = 0x14u;

        void added(uint8_t *rdram, R5900Context *, PS2Runtime *, const GemAdded &added)
        {
            s_shares.erase(added.gem);
            if (!playingCustom() || added.game == 0u)
                return;
            const guest::GameGem game(rdram, added.game);
            const uint8_t frets = game.slots();
            const Gh2Gem *chart = chartGem(added.track, static_cast<uint32_t>(game.tick()), frets);
            if (!chart || !chart->split())
                return;
            const float longest = *std::max_element(chart->laneMs.begin(), chart->laneMs.end());
            std::vector<float> &shares = s_shares[added.gem];
            for (uint32_t lane = 0; lane < 5u; ++lane)
                if ((frets >> lane) & 1u)
                    shares.push_back(chart->laneMs[lane] / longest);
        }

        // Gem::ApplyDuration(float start, float end, float far).
        PS2Runtime::RecompiledFunction s_applyDuration = nullptr;
        void applyDuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            const auto found = s_shares.find(gem);
            if (found == s_shares.end())
                return s_applyDuration(rdram, ctx, runtime);
            const float start = ctx->f[12], end = ctx->f[13], far = ctx->f[14];
            const float length = load<float>(rdram, gem + kGemTo) - load<float>(rdram, gem + kGemFrom);
            size_t k = 0;
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), last = load<uint32_t>(rdram, gem + gem::kRepsEnd);
                 r < last; r += 4u, ++k)
            {
                const float share = k < found->second.size() ? found->second[k] : 1.0f;
                const float ends = std::min(end, length * share);
                ctx->f[12] = std::min(start, ends);
                ctx->f[13] = ends;
                ctx->f[14] = far;
                runtime->callGuestFunction(rdram, ctx, s_addresses->gemRepSetDuration, {load<uint32_t>(rdram, r)});
            }
            ctx->pc = GPR_U32(ctx, 31);
        }

        void gone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_shares.clear();
        }
    }

    void installSustainTails(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        onGemAdded(added);
        replaceKeeping(runtime, addresses.gemApplyDuration, &applyDuration, s_applyDuration);
        onTrackGone(gone);
    }
}
