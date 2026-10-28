// song_progress: how far through the song it is, a small tube under the rock
// meter (ui/dta/song_progress.dta). Script makes it as the HUD enters and
// hands over its fill, glow and head; each frame the fill and glow are drawn
// out to the song's seconds over its length and the head put at their end.

#include "ui/song_progress.h"

#include "dta.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "script.h"

#include "ps2_runtime_macros.h"

#include <algorithm>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        // The fill's and its glow's meshes and their x scale at the song's
        // end; the head's mesh and its x at the start.
        uint32_t s_fill = 0u;
        uint32_t s_glow = 0u;
        float s_full = 0.0f;
        uint32_t s_head = 0u;
        float s_headX = 0.0f;
        // Whether the HUD shows its meters: not until they start to slide in.
        bool s_meters = true;

        // A Mesh's RndTransformable, its local xfm there: with no turn, the
        // x axis's first float the x scale, the position after three rows.
        constexpr uint32_t kLocal = ::milo::mesh::kTransform + ::milo::transformable::kLocal;
        constexpr uint32_t kXScale = kLocal;
        constexpr uint32_t kX = kLocal + 0x30u;
        // SongDB::GetSongDurationMs (0x11d2e0) reads it at +0x18.
        constexpr uint32_t kDurationMs = 0x18u;

        // A Mesh as script finds it is its Hmx::Object, a virtual base.
        uint32_t mesh(const script::Call &call, int i)
        {
            const uint32_t object = call.object(i);
            return object > ::milo::mesh::kObjectBase ? object - ::milo::mesh::kObjectBase : 0u;
        }

        // {ghc_song_progress <fill> <x scale full> <glow> <head>}: the head
        // where script put it, its middle on the fill's left end.
        script::Node songProgress(const script::Call &call)
        {
            s_fill = mesh(call, 1);
            s_full = call.number(2);
            s_glow = mesh(call, 3);
            s_head = mesh(call, 4);
            // The glow soft to its left, top and bottom but not its right,
            // where the head is: its mat's uvs through tex_xfm, half the
            // texture along it, its edge to its bright middle. The quad's u
            // runs right to left, so the half from the middle on.
            if (const uint32_t mat = s_glow ? load<uint32_t>(call.rdram, s_glow + ::milo::mesh::kMat) : 0u)
            {
                store<uint32_t>(call.rdram, mat + ::milo::mat::kTexGen, ::milo::mat::kTexGenXfmOrigin);
                store<float>(call.rdram, mat + ::milo::mat::kTexXfm, 0.5f);
                store<float>(call.rdram, mat + ::milo::mat::kTexXfm + 0x30u, 0.5f);
            }
            s_headX = s_head ? load<float>(call.rdram, s_head + kX) : 0.0f;
            return {};
        }

        // {ghc_song_progress_meters}: whether the meters show, so the bar
        // made does as they do.
        script::Node songProgressMeters(const script::Call &)
        {
            return {s_meters ? 1u : 0u, script::kInt};
        }

        // {ghc_with_this <object> <func>}: {<func>} run as the object's own
        // handlers are, $this that object, so {new} names into its dir, where
        // its scene draws what is made; then $this put back.
        script::Node withThis(const script::Call &call)
        {
            const uint32_t was = load<uint32_t>(call.rdram, s_addresses->dataThis);
            call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->dataSetThis, {call.object(1)});
            script::run(call.rdram, call.ctx, call.runtime, "{" + call.symbol(2) + "}");
            call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->dataSetThis, {was});
            return {};
        }

        // The song's seconds over its length, 0 to 1: TaskMgr's seconds,
        // the song's clock (its clocks at [+0x28], seconds the first, now at
        // +0xC), which stops while paused.
        float along(uint8_t *rdram)
        {
            const uint32_t songDb = load<uint32_t>(rdram, s_addresses->theSongDb);
            const float length = songDb ? load<float>(rdram, songDb + kDurationMs) / 1000.0f : 0.0f;
            if (!(length > 0.0f))
                return 0.0f;
            const uint32_t clocks = load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u);
            return std::clamp(load<float>(rdram, clocks + 0xcu) / length, 0.0f, 1.0f);
        }

        // A mesh's local float at `at` set, and the mesh marked moved; left
        // alone when as it was.
        void put(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t mesh, uint32_t at, float value)
        {
            if (!mesh || load<float>(rdram, mesh + at) == value)
                return;
            store<float>(rdram, mesh + at, value);
            runtime->callGuestFunction(rdram, ctx, s_addresses->setDirty, {mesh + ::milo::mesh::kTransform});
        }

        // Each frame of a song, as the HUD gives each score display its
        // score: the fill drawn out and the head at its end.
        struct SetScoreTag;
        void onSetScore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!s_fill)
                return;
            const EntryArgs args(ctx);
            const float done = along(rdram);
            // At least a sliver: a scale of 0 leaves the xfm singular.
            const float scale = std::max(0.001f, s_full * done);
            put(rdram, ctx, runtime, s_fill, kXScale, scale);
            put(rdram, ctx, runtime, s_glow, kXScale, scale);
            put(rdram, ctx, runtime, s_head, kX, s_headX + s_full * 16.0f * done);
            args.restore(ctx);
        }

        // As the HUD hides its meters for the intro and shows them to slide
        // in: the bar too, which they leave alone, though it pokes out past
        // the rock meter where it waits off screen.
        struct SetMetersShowingTag;
        void onSetMetersShowing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool showing = GPR_U32(ctx, 5) != 0u;
            if (showing == s_meters)
                return;
            s_meters = showing;
            if (!s_fill)
                return;
            const EntryArgs args(ctx);
            script::run(rdram, ctx, runtime, showing ? "{song_progress_show TRUE}" : "{song_progress_show FALSE}");
            args.restore(ctx);
        }

        struct HudEnterTag;
        void onHudEnter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            s_fill = s_glow = s_head = 0u;
            script::run(rdram, ctx, runtime, "{song_progress_enter}");
            args.restore(ctx);
        }
    }

    void installSongProgress(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        script::runWhenUiReady(kSongProgressDta);
        script::addCommand("ghc_song_progress", songProgress);
        script::addCommand("ghc_song_progress_meters", songProgressMeters);
        script::addCommand("ghc_with_this", withThis);
        EntryHook<HudEnterTag>::install(runtime, addresses.hudPanelEnter, onHudEnter);
        EntryHook<SetScoreTag>::install(runtime, addresses.bandScoreDisplaySetScore, onSetScore);
        EntryHook<SetMetersShowingTag>::install(runtime, addresses.hudPanelSetMetersShowing, onSetMetersShowing);
    }
}
