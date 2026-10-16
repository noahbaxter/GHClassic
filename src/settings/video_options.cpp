// The game's widescreen option, held by the host instead of the save.
//
// Options::SyncVideoOptions (0x10db80) passes mWideScreen (+0x38) to
// Options::SetWideScreen (0x10de58), which calls TheRnd->SetAspect with 2
// for 16:9 or 1 for 4:3: Rnd::YRatio then squeezes every camera's projection
// and the host shows the picture at that aspect. Its callers are
// Campaign::Load (the save load at boot) and the options DTA handler, so the
// flag goes in on entry and a save cannot turn it back.

#include "settings/video_options.h"

#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "script.h"
#include "settings/settings.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <string>

namespace gh2
{
    namespace
    {
        constexpr uint32_t kWideScreen = 0x38u; // Options::mWideScreen

        struct SyncTag;

        void onSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            store<uint32_t>(rdram, GPR_U32(ctx, 4) + kWideScreen, settings::get(settings::kWidescreen) ? 1u : 0u);
        }

        int s_mostMsaa = 16;

        // The Video Settings rows (ui/dta/game_settings.dta). {video <row>}
        // is the row's setting, {video <row> next} and {video <row> prev}
        // put it to the value after or before and round, and {video <row>
        // set <n>} to that one; each gives what it then is. The renderer
        // takes each as it changes.
        script::Node videoCommand(const script::Call &call)
        {
            const std::string row = call.symbol(1);
            const std::string does = call.size() > 2 ? call.symbol(2) : "";
            const int step = does == "next" ? 1 : does == "prev" ? -1 : 0;
            if (row == "msaa")
            {
                // 1, 2, 4 and on to the most the GPU draws.
                int at = 0, count = 1;
                while ((1 << at) < msaaSamples())
                    ++at;
                while ((1 << count) <= s_mostMsaa)
                    ++count;
                if (step != 0)
                    settings::set(settings::kMsaa, 1 << ((at + step + count) % count));
                if (does == "set")
                    settings::set(settings::kMsaa, static_cast<int>(call.number(3)));
                return {static_cast<uint32_t>(msaaSamples()), script::kInt};
            }
            if (row == "render_scale")
            {
                // By 25 from 50 to 200.
                const int at = settings::get(settings::kRenderScale) + 25 * step;
                if (step != 0)
                    settings::set(settings::kRenderScale, at > 200 ? 50 : at < 50 ? 200 : at);
                if (does == "set")
                    settings::set(settings::kRenderScale, static_cast<int>(call.number(3)));
                return {static_cast<uint32_t>(settings::get(settings::kRenderScale)), script::kInt};
            }
            if (row == "perf_overlay")
            {
                // host/perf.h's four levels.
                if (step != 0)
                    settings::set(settings::kPerfOverlay, (settings::get(settings::kPerfOverlay) + step + 4) % 4);
                if (does == "set")
                    settings::set(settings::kPerfOverlay, static_cast<int>(call.number(3)));
                return {static_cast<uint32_t>(settings::get(settings::kPerfOverlay)), script::kInt};
            }
            const settings::Key key = row == "temporal_aa"    ? settings::kTemporalAa
                                      : row == "lighting"     ? settings::kLighting
                                      : row == "vector_fonts" ? settings::kVectorFonts
                                                              : settings::kHighDetail;
            if (step != 0)
                settings::set(key, !settings::get(key));
            if (does == "set")
                settings::set(key, static_cast<int>(call.number(3)));
            return {static_cast<uint32_t>(settings::get(key)), script::kInt};
        }
    }

    void setMostMsaa(int most)
    {
        s_mostMsaa = most;
    }

    int msaaSamples()
    {
        int samples = 1;
        while (samples * 2 <= std::min(settings::get(settings::kMsaa), s_mostMsaa))
            samples *= 2;
        return samples;
    }

    void installVideoOptions(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<SyncTag>::install(runtime, addresses.optionsSyncVideo, onSync);
        script::addCommand("video", videoCommand);
        // Widescreen keeps the whole crowd.
        script::runWhenUiReady(kCrowdDta);
    }
}
