// GH1's CharFace (GH1 0x2a6cb0-0x2a7930), which GH2 dropped, on GH1's face
// morphs that gh1.cpp brings over as gh1_face.mrf and gh1_lashes.mrf.
//
// A morph keys each pose at its own frame (ref 0, bad01 1 to blink 9), so a
// frame is a pose. Every pose_length seconds CharFace::ExcitementPicker picks
// one at random from the game's excitement level, never the one it has, and
// SimpleBlender eases to it over blend_time. Both morphs in a character share
// one picker, as GH1's chained them under one. GH2's RndMorph::SetFrame
// (0x201048) does the blending and syncs the target mesh.

#include "gh1_face.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"

#include <cmath>
#include <cstring>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        // GH1's charsys.dta HERO_FACES: poses by frame, blend_time 0.5,
        // pose_length 1, and excitement_poses.
        constexpr float kBlendTime = 0.5f;
        constexpr float kPoseLength = 1.0f;
        constexpr int kOkay = 2;
        const std::vector<int> kExcitementPoses[] = {
            {1, 2, 3},       // kExcitementBoot: bad01-03
            {1, 2, 3},       // kExcitementBad
            {0, 4},          // kExcitementOkay: ref, good01
            {4, 5, 6, 7, 8}, // kExcitementGreat: good01-05
            {4, 5, 6, 7, 8}, // kExcitementPeak
        };

        // SimpleBlender: the pose shown, the one it eases to, and how far.
        struct Face
        {
            float timer = 0.0f;
            int pose = 0;
            int next = 0;
            float blend = 1.0f;

            // SimpleBlender::SetNewPose (GH1 0x2a7740): mid-blend back to the
            // pose it left runs the blend in reverse.
            void setNewPose(int p)
            {
                if (blend != 1.0f && pose == p)
                {
                    pose = next;
                    next = p;
                    blend = 1.0f - blend;
                }
                else if (blend == 1.0f ? pose != p : next != p)
                {
                    next = p;
                    blend = 0.0f;
                }
            }
        };

        const Addresses *s_addresses = nullptr;
        std::set<uint32_t> s_morphs;
        std::unordered_map<uint32_t, Face> s_faces; // by the morphs' dir
        // The level a song set this frame, if one did. Out of songs it is
        // Okay, as GH1 starts each one (ArenaPanel::Start, GH1 0x10e460).
        std::optional<int> s_polled;
        int s_excitement = kOkay;
        std::minstd_rand s_random;

        struct CtorTag;
        void onCtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_morphs.insert(GPR_U32(ctx, 4));
        }

        struct DtorTag;
        void onDtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_morphs.erase(GPR_U32(ctx, 4));
        }

        // GamePanel::Poll sets it each frame of a song (0x106e78).
        struct ExcitementTag;
        void onExcitement(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            s_polled = static_cast<int>(GPR_U32(ctx, 5));
        }

        // TaskMgr::DeltaSeconds (0x2c67f0): its timer's (+0x28) now less
        // its last.
        float deltaSeconds(uint8_t *rdram)
        {
            const uint32_t timer = load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u);
            return load<float>(rdram, timer + 0xcu) - load<float>(rdram, timer + 0x10u);
        }

        // CharFace::ExcitementPicker (GH1 0x2a7518) then SimpleBlender::Poll
        // (GH1 0x2a77a0).
        void poll(Face &face, float dt)
        {
            face.timer += dt;
            if (face.timer > kPoseLength)
            {
                face.timer = 0.0f;
                const std::vector<int> &poses =
                    kExcitementPoses[s_excitement >= 0 && s_excitement < 5 ? s_excitement : kOkay];
                size_t i = std::uniform_int_distribution<size_t>(0u, poses.size() - 1u)(s_random);
                if (poses[i] == face.pose)
                    i = (i + 1u) % poses.size();
                face.setNewPose(poses[i]);
            }
            if (face.blend != 1.0f)
            {
                face.blend += dt / kBlendTime;
                if (face.blend > 1.0f)
                {
                    face.blend = 1.0f;
                    face.pose = face.next;
                }
            }
        }

        void setFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t morph, float frame,
                      float blend)
        {
            ctx->f[12] = frame;
            ctx->f[13] = blend;
            runtime->callGuestFunction(rdram, ctx, s_addresses->rndMorphSetFrame, {morph});
        }

        // SimpleBlender::Draw (GH1 0x2a78a0): the pose, then the next eased
        // in by a half cosine.
        void draw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t morph, const Face &face)
        {
            setFrame(rdram, ctx, runtime, morph, static_cast<float>(face.pose), 1.0f);
            if (face.blend != 1.0f)
                setFrame(rdram, ctx, runtime, morph, static_cast<float>(face.next),
                         0.5f - 0.5f * std::cos(face.blend * 3.14159265f));
        }

        struct BeginTag;
        void onBeginDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            s_excitement = s_polled.value_or(kOkay);
            s_polled.reset();
            std::unordered_map<uint32_t, std::vector<uint32_t>> morphs;
            for (const uint32_t morph : s_morphs)
            {
                const uint32_t object = load<uint32_t>(rdram, morph);
                const uint32_t name = load<uint32_t>(rdram, object + milo::object::kName);
                const uint32_t dir = load<uint32_t>(rdram, object + milo::object::kDir);
                if (name != 0u && dir != 0u && std::strncmp(reinterpret_cast<const char *>(getMemPtr(rdram, name)), "gh1_", 4) == 0)
                    morphs[dir].push_back(morph);
            }
            std::erase_if(s_faces, [&](const auto &face) { return !morphs.count(face.first); });
            if (morphs.empty())
                return;
            const float dt = deltaSeconds(rdram);
            const EntryArgs args(ctx);
            for (const auto &[dir, list] : morphs)
            {
                Face &face = s_faces[dir];
                poll(face, dt);
                for (const uint32_t morph : list)
                    draw(rdram, ctx, runtime, morph, face);
            }
            args.restore(ctx);
        }
    }

    void installGh1Face(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<CtorTag>::install(runtime, addresses.rndMorphCtor, onCtor);
        EntryHook<DtorTag>::install(runtime, addresses.rndMorphDtor, onDtor);
        EntryHook<ExcitementTag>::install(runtime, addresses.gamePanelSetExcitementLevel, onExcitement);
        EntryHook<BeginTag>::install(runtime, addresses.psRndBeginDrawing, onBeginDrawing);
    }
}
