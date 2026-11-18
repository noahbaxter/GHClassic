// GH2 makes every gem star power's (type 2) while it is on, a phrase's too;
// Gem::SetType and Gem::AddRep make a phrase's a star (1) instead, and
// GemRep::SetType its reps' star body and HOPO top their cyan_ copies, put
// back as they were on any other type. Every lane's star is the red lane's
// (the lanes' differ only in their block of stargem.tex), whose faces are
// the most apart in light, its block turned cyan as bright as it was, and a
// HOPO's top the blue one, as star power's blue HOPO has. Its tail and glow
// are star power's (tail_bonus, tail_glow_bonus), not a phrase's.

#include "highway/cyan_stars.h"

#include "guest.h"
#include "highway/extended/guitar/open_gems.h"
#include "highway/gem_looks.h"
#include "hook.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "render/hsv.h"
#include "settings/settings.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace gh2::guitar
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // The Gems in a star phrase still to be won.
        std::set<uint32_t> s_phraseGems;

        // A gem added to the track, a star if it is in a star phrase still
        // to be won (0x1568c0); under star power every one is star power's,
        // so whether it is is kept here.
        void gemAdded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const GemAdded &added)
        {
            const uint32_t db = load<uint32_t>(rdram, s_addresses->theSongDb);
            const bool in =
                runtime->callGuestFunction(rdram, ctx, s_addresses->songDbIsInStarPhrase, {db, added.track, added.index}) != 0u;
            const bool won = load<uint32_t>(rdram, added.manager + gemManager::kPhraseLost) == 0u ||
                             runtime->callGuestFunction(rdram, ctx, s_addresses->songDbIsStarPhraseStart,
                                                        {db, added.track, added.index}) != 0u;
            if (in && won)
                s_phraseGems.insert(added.gem);
            else
                s_phraseGems.erase(added.gem);
        }

        // GemManager::OnMissPhrase(int gem): the phrase from that gem on is
        // lost, and its gems on the track (from +0x20 to the next to add) turn
        // plain where star power is off (+0x2f4). Where it is on, GH2 leaves
        // them, so a cyan star is turned star power's here.
        struct MissPhraseTag;
        void onMissPhrase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t manager = args.a[0];
            const uint32_t db = load<uint32_t>(rdram, s_addresses->theSongDb);
            const uint32_t track = trackNum(rdram, ctx, runtime, load<uint32_t>(rdram, manager + gemManager::kConfig));
            const bool bonus = load<uint32_t>(rdram, manager + gemManager::kBonus) != 0u;
            const auto next = static_cast<int32_t>(load<uint32_t>(rdram, manager + gemManager::kNext));
            for (auto i = std::max(static_cast<int32_t>(load<uint32_t>(rdram, manager + gemManager::kFirst)),
                                   static_cast<int32_t>(args.a[1]));
                 i < next; ++i)
            {
                const auto index = static_cast<uint32_t>(i);
                if (!runtime->callGuestFunction(rdram, ctx, s_addresses->songDbIsInStarPhrase, {db, track, index}))
                    break;
                const uint32_t gem = load<uint32_t>(rdram, manager + gemManager::kGems) + index * gemManager::kGemSize;
                if (s_phraseGems.erase(gem) && bonus && !(load<uint32_t>(rdram, gem + gem::kFrets) & gem::kHitOrPassed))
                    runtime->callGuestFunction(rdram, ctx, s_addresses->gemSetType, {gem, gemRep::kTypeBonus});
                if (runtime->callGuestFunction(rdram, ctx, s_addresses->songDbIsStarPhraseEnd, {db, track, index}))
                    break;
            }
            args.restore(ctx);
        }

        // The cyan stars' pieces in the track's scene.
        constexpr const char *kCyan = "cyan_";
        constexpr const char *kLanes[] = {"green", "red", "yellow", "blue", "orange"};
        constexpr const char *kCyanStarFrom = "red_star.mesh", *kCyanTopFrom = "blue_top_star.mesh";
        // Their light over the red star's (Rec. 601 luma): a plain hue turn
        // makes cyan twice as light and flat.
        constexpr float kStarLift = 1.5f;

        void addPieces(milo::Dir &scene)
        {
            const auto luma = [](const float *c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; };
            int made = addCopy(scene, "stargem.tex", "cyan_stargem.tex", [&](milo::Bytes &body)
                               {
                                   return recolor(body, scene.version, [&](float *rgb)
                                                  {
                                                      float out[3];
                                                      fromHsv(kStarPowerHue / 60.0f, toHsv(rgb).s, 1.0f, out);
                                                      const float scale = luma(out) > 0.0f ? luma(rgb) * kStarLift / luma(out) : 0.0f;
                                                      for (int c = 0; c < 3; ++c)
                                                          rgb[c] = out[c] * scale;
                                                  });
                               });
            made += addCopy(scene, "star.mat", "cyan_star.mat",
                            [](milo::Bytes &body) { return milo::replaceStr(body, "stargem.tex", "cyan_stargem.tex"); });
            for (const char *lane : kLanes)
                for (const auto &[source, part, cyanMat] : {std::tuple{kCyanStarFrom, "_star.mesh", "cyan_star.mat"},
                                                            std::tuple{kCyanTopFrom, "_top_star.mesh", ""}})
                {
                    const std::string as = kCyan + std::string(lane) + part;
                    made += addCopy(scene, source, as, [&](milo::Bytes &body)
                                    {
                                        return milo::replaceStr(body, source, as) &&
                                               (*cyanMat == '\0' || milo::replaceStr(body, "star.mat", cyanMat));
                                    });
                }
            const size_t pieces = 2u + 2u * std::size(kLanes);
            if (made != static_cast<int>(pieces))
                std::cerr << "[gameplay] cyan stars: " << made << " of " << pieces << " pieces made" << std::endl;
        }

        // The reps to be cyan stars (applyCyan), and each one's parts'
        // templates as they were, by part, and the glows of those with a
        // sustain.
        std::set<uint32_t> s_cyanWanted, s_cyanLines;
        std::map<uint32_t, std::map<uint32_t, uint32_t>> s_cyanReps;
        // Whether the gem Gem::AddRep adds is a cyan star, for every rep it
        // makes, a chord's one a fret (AddRepForSlot), each typed by
        // GemRep::Init, from where SetType returns to here.
        bool s_addingCyan = false;
        constexpr uint32_t kFromInit = 0x1586d4u;

        bool cyanStar(uint32_t gem, uint32_t type)
        {
            return type == gemRep::kTypeBonus && s_phraseGems.count(gem) != 0u;
        }

        void restoreCyan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep)
        {
            if (const uint32_t line = load<uint32_t>(rdram, rep + gemRep::kGlow))
                s_cyanLines.erase(line);
            const auto it = s_cyanReps.find(rep);
            if (it == s_cyanReps.end())
                return;
            for (const auto &[part, was] : it->second)
                setPartTemplate(rdram, ctx, runtime, rep, part, was);
            s_cyanReps.erase(it);
        }

        // A cyan star's tail star power's (tail_bonus), and its glow
        // (onGlowDraw); SetType gives the next type's back.
        constexpr const char *kStarGlow = "tail_glow_star.mat", *kBonusGlow = "tail_glow_bonus.mat",
                             *kBonusTail = "tail_bonus.mat";
        void cyanTail(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep)
        {
            if (const uint32_t line = load<uint32_t>(rdram, rep + gemRep::kGlow))
                s_cyanLines.insert(line);
            const uint32_t tail = load<uint32_t>(rdram, rep + gemRep::kTail);
            const uint32_t mat = tail ? load<uint32_t>(rdram, tail + ::milo::mesh::kMat) : 0u;
            const uint32_t scene = mat ? load<uint32_t>(rdram, mat + ::milo::object::kDir) : 0u;
            const uint32_t bonus = scene ? findObject(rdram, ctx, runtime, scene, kBonusTail) : 0u;
            if (bonus != 0u && bonus != mat)
                runtime->callGuestFunction(rdram, ctx, s_addresses->rndMeshSetMat, {tail, bonus});
        }

        void applyCyan(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t rep, bool want)
        {
            if (!want || load<uint32_t>(rdram, rep + gemRep::kType) != gemRep::kTypeStar)
                return restoreCyan(rdram, ctx, runtime, rep);
            cyanTail(rdram, ctx, runtime, rep);
            for (const uint32_t part : {gemRep::kStarTop, gemRep::kStarBody})
            {
                const uint32_t object = partObject(rdram, rep, part);
                const std::string name(objectName(rdram, object));
                if (object == 0u || name.rfind(kCyan, 0u) == 0u)
                    continue;
                // None for a strummed star's black top, which stays.
                const uint32_t scene = load<uint32_t>(rdram, object + ::milo::object::kDir);
                const uint32_t found = findObject(rdram, ctx, runtime, scene, kCyan + name);
                if (found == 0u)
                    continue;
                auto &was = s_cyanReps[rep];
                if (!was.count(part))
                    was[part] = partTemplate(rdram, rep, part);
                // A multimesh, as the lane's is, turning as it does.
                const uint32_t source =
                    findObject(rdram, ctx, runtime, scene, part == gemRep::kStarBody ? kCyanStarFrom : kCyanTopFrom);
                showPartAs(rdram, ctx, runtime, rep, part, found, true, source);
            }
        }

        // Gem::SetType(GemRep::Type): each of its reps' type.
        struct GemSetTypeTag;
        void onGemSetType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            if (load<uint8_t>(rdram, gem + gem::kFrets) == 0u)
                return;
            const bool want = cyanStar(gem, GPR_U32(ctx, 5));
            if (want)
                SET_GPR_U32(ctx, 5, gemRep::kTypeStar);
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), end = load<uint32_t>(rdram, gem + gem::kRepsEnd);
                 r < end; r += 4u)
                if (want)
                    s_cyanWanted.insert(load<uint32_t>(rdram, r));
                else
                    s_cyanWanted.erase(load<uint32_t>(rdram, r));
        }

        // Gem::AddRep(GemRepPool &, RndGroup *, GemRep::Type, const
        // TrackConfig &).
        PS2Runtime::RecompiledFunction s_addRep = nullptr, s_setType = nullptr, s_enableDuration = nullptr;
        void addRep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t gem = GPR_U32(ctx, 4);
            s_addingCyan = load<uint8_t>(rdram, gem + gem::kFrets) != 0u && cyanStar(gem, GPR_U32(ctx, 7));
            if (s_addingCyan)
                SET_GPR_U32(ctx, 7, gemRep::kTypeStar);
            s_addRep(rdram, ctx, runtime);
        }

        // GemRep::SetType(GemRep::Type), which takes its template's parts.
        void setType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t rep = GPR_U32(ctx, 4), from = GPR_U32(ctx, 31);
            callOriginal(rdram, ctx, runtime, s_addresses->gemRepSetType, s_setType);
            if (!isOpenRep(rep))
            {
                if (from == kFromInit && s_addingCyan)
                    s_cyanWanted.insert(rep);
                else if (from == kFromInit)
                    s_cyanWanted.erase(rep);
                applyCyan(rdram, ctx, runtime, rep, s_cyanWanted.count(rep) != 0u);
            }
        }

        // GemRep::EnableDuration, which lends the rep a tail.
        void enableDuration(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t rep = GPR_U32(ctx, 4);
            callOriginal(rdram, ctx, runtime, s_addresses->gemRepEnableDuration, s_enableDuration);
            if (s_cyanReps.count(rep))
                cyanTail(rdram, ctx, runtime, rep);
        }

        // Gem::RemoveRep(GemRepPool &): its reps go back to the pool.
        struct RemoveRepTag;
        void onRemoveRep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t gem = args.a[0];
            for (uint32_t r = load<uint32_t>(rdram, gem + gem::kReps), end = load<uint32_t>(rdram, gem + gem::kRepsEnd);
                 r < end; r += 4u)
            {
                const uint32_t rep = load<uint32_t>(rdram, r);
                s_cyanWanted.erase(rep);
                restoreCyan(rdram, ctx, runtime, rep);
            }
            args.restore(ctx);
        }

        // As the track goes: its reps are deleted with it, none to put back.
        void trackGone(uint8_t *, R5900Context *, PS2Runtime *, uint32_t)
        {
            s_cyanWanted.clear(), s_cyanReps.clear(), s_cyanLines.clear(), s_phraseGems.clear();
            s_addingCyan = false;
        }

        // RndLine::DrawShowing(DrawType): a cyan star's glow star power's, as
        // its gem is, not a phrase's. RndLine: its Mat an ObjPtr at +0x128,
        // the pointer 8 into it (SetMat, 0x1bc980).
        constexpr uint32_t kLineMat = 0x130u;
        struct GlowDrawTag;
        void onGlowDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const uint32_t line = args.a[0];
            const uint32_t mat = load<uint32_t>(rdram, line + kLineMat);
            if (!s_cyanLines.count(line) || objectName(rdram, mat) != kStarGlow)
                return;
            const uint32_t scene = load<uint32_t>(rdram, mat + ::milo::object::kDir);
            if (const uint32_t bonus = scene ? findObject(rdram, ctx, runtime, scene, kBonusGlow) : 0u)
                runtime->callGuestFunction(rdram, ctx, s_addresses->rndLineSetMat, {line, bonus});
            args.restore(ctx);
        }
    }

    bool inStarPhrase(uint32_t gem)
    {
        return s_phraseGems.count(gem) != 0u;
    }

    void installCyanStars(PS2Runtime &runtime, const Addresses &addresses)
    {
        if (!settings::get(settings::kStarPowerOverlap))
            return;
        s_addresses = &addresses;
        onGemAdded(gemAdded);
        EntryHook<MissPhraseTag>::install(runtime, addresses.gemManagerOnMissPhrase, onMissPhrase);
        addToTrackScene(addPieces);
        EntryHook<GemSetTypeTag>::install(runtime, addresses.gemSetType, onGemSetType);
        replaceKeeping(runtime, addresses.gemAddRep, &addRep, s_addRep);
        replaceKeeping(runtime, addresses.gemRepSetType, &setType, s_setType);
        replaceKeeping(runtime, addresses.gemRepEnableDuration, &enableDuration, s_enableDuration);
        EntryHook<RemoveRepTag>::install(runtime, addresses.gemRemoveRep, onRemoveRep);
        onTrackGone(trackGone);
        EntryHook<GlowDrawTag>::install(runtime, addresses.rndLineDrawShowing, onGlowDraw);
    }
}
