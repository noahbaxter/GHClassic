// NowBar::Hit(float, int gem, bool, PhraseState) hits each of the gem's slots
// (GameGem +0x0c, read at 0x15d138): its smasher pushed up (GemSmasher::Hit,
// 0x15d18c) and its effects started, the burst, burning on while the gem is
// held. An open has none, so it is lent all five for Hit, given back as Hit
// sets its effects' type (SmashFX::SetType, the call after).
//
// NowBar's SlotViews each draw an effect at every slot that started one
// (StartInstance, 0x15f258), placing it first (SetWorldXfm): from Hit, the
// burst at +0x20, the burn at +0x24, and a phrase's flames, and the lightning
// on its last gem, at +0x28. An open's burst is a lane's at every lane,
// purple; its burn one wide one at the middle, the rest hidden; its flames
// and lightning star power's cyan, never purple: NowBar draws them last, so
// purple there stays on what is drawn after.

#include "highway/extended/guitar/open_bursts.h"

#include "gameplay/extended/guitar/gem_kinds.h"
#include "gameplay/extended/guitar/hammer_ons.h"
#include "guest.h"
#include "guest/beatmatch.h"
#include "highway/extended/guitar/open_gems.h"
#include "highway/gem_looks.h"
#include "hook.h"
#include "render/draw_hue.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // The flames' hue turn (fire, red to yellow, to violet to pink), the
        // burn drawn this many times wider, its particles rising from this
        // much wider, and its sprites this many times as wide and as tall
        // (GH2's billboards keep their size however their place is
        // stretched).
        constexpr float kFlameHue = 250.0f, kFlameStretch = 3.5f, kBurnSpread = 28.0f;
        constexpr float kFlameWide = 3.0f, kFlameTall = 1.5f;

        // SlotViews' five slots' views (RndGroup *, 0 for none), each one's
        // RndTransformable at +0x50.
        constexpr uint32_t kEverySlot = 0x1fu, kSlots = 5u, kViewTransformable = 0x50u;
        struct Views
        {
            uint32_t at;
            bool wide, purple;
        };
        constexpr Views kNowBarViews[] = {{0x20u, false, true}, {0x24u, true, true}, {0x28u, false, false}};

        // Each SlotViews' slots whose effect is an open's, and those whose
        // open effects are wide and purple.
        std::map<uint32_t, uint8_t> s_openSlots;
        std::set<uint32_t> s_wideViews, s_purpleViews;
        // The open gem lent every slot for Hit, 0 for none, and the slots it
        // had (one hammered on has the middle lane's, from gem_kinds).
        uint32_t s_lentSlots = 0u;
        uint8_t s_lentWas = 0u;

        // The burn's particles (SmashFX's BurnFX, drawn at the slot with the
        // burst) rise from a lane's middle, a draw stretched wide only
        // spreading them a little: an open's from across the bar, their
        // boxes (RndParticleSys +0x140 and +0x150, as set_pos sets them,
        // 0x1d2610) wide. A PsParticleSys's Hmx::Object virtual base is at
        // +0x2e0 (0x3523a4). They are the one track scene's, which every
        // slot of every track draws, made as the game polls: wide while
        // either track last hit an open, and drawn narrow again at the
        // other's slots (onPlaceView).
        constexpr const char *kBurnParts[] = {"burn_tail_big.part", "burn_tail_base.part"};
        constexpr uint32_t kBurnBoxes[] = {0x140u, 0x150u}, kPartObjectBase = 0x2e0u;
        struct Burn
        {
            uint32_t part;
            float x[2];
        };
        std::vector<Burn> s_burns;
        uint32_t s_burnScene = 0u;
        std::map<uint32_t, bool> s_openBurns; // by track, its last hit an open's
        bool s_burnWide = false;

        void widenBurn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool open)
        {
            s_burnWide = open;
            const uint32_t scene = openScene();
            if (scene != s_burnScene)
            {
                s_burns.clear();
                s_burnScene = scene;
                for (const char *name : kBurnParts)
                    if (const uint32_t found = scene ? findObject(rdram, ctx, runtime, scene, name) : 0u)
                    {
                        const uint32_t part = found - kPartObjectBase;
                        s_burns.push_back({part, {load<float>(rdram, part + kBurnBoxes[0]), load<float>(rdram, part + kBurnBoxes[1])}});
                    }
            }
            for (const Burn &burn : s_burns)
                for (uint32_t b = 0; b < 2u; ++b)
                    store<float>(rdram, burn.part + kBurnBoxes[b], open ? burn.x[b] * kBurnSpread : burn.x[b]);
        }

        // NowBar::Hit: an open's gem lent every slot, and each SlotViews'
        // slots an open's until another gem is hit there.
        struct NowBarHitTag;
        void onNowBarHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!playingCustom())
                return;
            const EntryArgs args(ctx);
            const uint32_t track = trackNum(rdram, ctx, runtime, load<uint32_t>(rdram, args.a[0] + nowBar::kConfig));
            const uint32_t gem = static_cast<uint32_t>(runtime->callGuestFunction(
                rdram, ctx, s_addresses->songDbGetGem, {load<uint32_t>(rdram, s_addresses->theSongDb), track, args.a[1]}));
            const uint8_t slots = gem != 0u ? guest::GameGem(rdram, gem).slots() : 0u;
            const bool open = gem != 0u && (slots == 0u || gem == hammeredOpen());
            s_openBurns[track] = open;
            widenBurn(rdram, ctx, runtime,
                      std::any_of(s_openBurns.begin(), s_openBurns.end(), [](const auto &burn) { return burn.second; }));
            args.restore(ctx);
            for (const Views &views : kNowBarViews)
            {
                const uint32_t self = load<uint32_t>(rdram, args.a[0] + views.at);
                uint8_t &mask = s_openSlots[self];
                mask = open ? kEverySlot : mask & ~slots;
                if (views.wide)
                    s_wideViews.insert(self);
                if (views.purple)
                    s_purpleViews.insert(self);
            }
            if (open)
            {
                s_lentWas = slots;
                s_lentSlots = gem;
                guest::GameGem(rdram, gem).setSlots(kEverySlot);
            }
        }

        struct SmashTypeTag;
        void onSmashType(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            if (s_lentSlots != 0u)
                guest::GameGem(rdram, s_lentSlots).setSlots(s_lentWas);
            s_lentSlots = 0u;
        }

        // As the track goes: its views and burn with it.
        void trackGone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_openSlots.clear(), s_wideViews.clear(), s_purpleViews.clear(), s_burns.clear(), s_openBurns.clear();
            s_burnScene = 0u, s_lentSlots = 0u, s_burnWide = false;
        }

        // The SlotViews drawing now, with open slots, and the slot it is at.
        uint32_t s_drawingViews = 0u, s_drawingSlot = 0u;

        // SlotViews::DrawShowing(DrawType) draws each slot's view in turn
        // (0x15f038); its look ends with it.
        PS2Runtime::RecompiledFunction s_slotViewsDraw = nullptr;
        void slotViewsDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const auto found = s_openSlots.find(self);
            s_drawingViews =
                (found != s_openSlots.end() && found->second != 0u) || (s_burnWide && s_wideViews.count(self)) ? self : 0u;
            s_drawingSlot = 0u;
            callOriginal(rdram, ctx, runtime, s_addresses->slotViewsDrawShowing, s_slotViewsDraw);
            s_drawingViews = 0u;
            setDrawHue(0.0f);
            setSpriteStretch(1.0f, 1.0f);
        }

        // SetWorldXfm, as SlotViews places a slot's view: an open slot's
        // purple, and a wide one's at the middle, its sprites too, and
        // nothing at the other lanes; a burn not an open's narrowed by as
        // much as the particles are widened. A slot is found by its view,
        // in order, as every slot may show the same one.
        struct PlaceViewTag;
        void onPlaceView(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (s_drawingViews == 0u)
                return;
            const uint32_t views = load<uint32_t>(rdram, s_drawingViews + slotViews::kViews);
            for (uint32_t s = s_drawingSlot; s < kSlots; ++s)
            {
                const uint32_t view = load<uint32_t>(rdram, views + s * 4u);
                if (view == 0u || view + kViewTransformable != GPR_U32(ctx, 4))
                    continue;
                s_drawingSlot = s + 1u;
                const bool open = (s_openSlots[s_drawingViews] >> s) & 1u;
                const bool burn = s_wideViews.count(s_drawingViews) != 0u;
                const bool middle = s == kOpenLane;
                setDrawHue(open && s_purpleViews.count(s_drawingViews) ? kFlameHue : 0.0f);
                if (!burn || (!open && !s_burnWide))
                    return setSpriteStretch(1.0f, 1.0f);
                setSpriteStretch(!open ? 1.0f : middle ? kFlameWide : 0.0f, !open ? 1.0f : middle ? kFlameTall : 0.0f);
                // Its place (SetWorldXfm's Transform: three axes, then
                // where) wide along its first, or none, or narrowed.
                const float by = !open ? 1.0f / kBurnSpread : middle ? kFlameStretch : 0.0f;
                for (uint32_t axis = 0; axis < (open && !middle ? 3u : 1u); ++axis)
                    for (uint32_t c = 0; c < 3u; ++c)
                    {
                        const uint32_t at = GPR_U32(ctx, 5) + axis * 16u + c * 4u;
                        store<float>(rdram, at, load<float>(rdram, at) * by);
                    }
                return;
            }
        }
    }

    void installOpenBursts(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<NowBarHitTag>::install(runtime, addresses.nowBarHit, onNowBarHit);
        EntryHook<SmashTypeTag>::install(runtime, addresses.smashFxSetType, onSmashType);
        replaceKeeping(runtime, addresses.slotViewsDrawShowing, &slotViewsDraw, s_slotViewsDraw);
        EntryHook<PlaceViewTag>::install(runtime, addresses.setWorldXfm, onPlaceView);
        onTrackGone(trackGone);
    }
}
