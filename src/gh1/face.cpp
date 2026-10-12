// GH1's CharFace (GH1 debug 0x2a6cb0-0x2a7930), which GH2 dropped, on GH1's
// face morphs that gh1/rig.cpp brings over as <outfit>_face.mrf and
// <outfit>_lashes.mrf, run by its archetype's face_data.
//
// GH1 addresses are retail's (SLUS_212.24); those marked debug are the
// symbolized debug build's, for functions retail's symbol map has no name
// for.
//
// A morph keys each pose at its own frame, its place in face_data's poses
// (ref 0, bad01 1 to blink 9), so a frame is a pose. Every pose_length seconds
// CharFace::ExcitementPicker picks one at random from the game's excitement
// level, never the one it has, and SimpleBlender eases to it over blend_time.
// Both morphs in a character share one picker, as GH1's chained them under
// one. GH2's RndMorph::SetFrame (0x201048) does the blending and syncs the
// target mesh. Only songs poll it (CharMan::Poll's one caller is
// ArenaPanel::Poll, GH1 debug 0x10e73c); elsewhere a face holds Reset's
// pose, ref.
//
// A singer's face has an event_list in place of excitement poses, and
// CharFace::EventPicker (GH1 0x287a80) shows the pose of the chart event it
// is in, looking event_offset ms ahead, and ref between them. The events are
// the gems track's note 108, their pose open (charsys.dta's singer_events).

#include "gh1/face.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        // An archetype's face_data, its pose names as frames.
        struct FaceData
        {
            std::map<int, std::vector<int>> excitementPoses;
            float blendTime = 0.0f;
            float poseLength = 1.0f; // CharFace::CharFace's (GH1 debug 0x2a6cb0)
            std::optional<int> sung; // the pose its events show
            float eventOffset = 0.0f; // seconds
        };

        // SimpleBlender: the pose shown, the one it eases to, and how far.
        struct Face
        {
            const FaceData *data = nullptr;
            float timer = 0.0f;
            int pose = 0;
            int next = 0;
            float blend = 1.0f;

            // SimpleBlender::SetNewPose (GH1 0x287dd8): mid-blend back to the
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
        std::map<std::string, FaceData> s_data; // by outfit
        std::map<std::string, std::vector<std::pair<float, float>>> s_singing; // by song
        constexpr uint32_t kSong = 0x268u; // GameConfig's, a Symbol (GetSongData, 0x1261e0)
        std::set<uint32_t> s_morphs;
        std::unordered_map<uint32_t, Face> s_faces; // by the morphs' dir
        // The level a song set this frame, if one did.
        std::optional<int> s_polled;
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

        // Whether the song playing has its singer's mouth open at the song's
        // seconds (TaskMgr's first clock) plus `ahead`.
        bool singing(uint8_t *rdram, float ahead)
        {
            const uint32_t config = load<uint32_t>(rdram, s_addresses->theGameConfig);
            const uint32_t song = config ? load<uint32_t>(rdram, config + kSong) : 0u;
            const auto events = song ? s_singing.find(reinterpret_cast<const char *>(getMemPtr(rdram, song))) : s_singing.end();
            if (events == s_singing.end())
                return false;
            const float at = load<float>(rdram, load<uint32_t>(rdram, s_addresses->theTaskMgr + 0x28u) + 0xcu) + ahead;
            const auto next = std::upper_bound(events->second.begin(), events->second.end(), std::pair(at, 1.0e9f));
            return next != events->second.begin() && std::prev(next)->second >= at;
        }

        // CharFace::EventPicker or ExcitementPicker (GH1 0x287a80,
        // 0x287b70), then SimpleBlender::Poll (GH1 0x287e38).
        void poll(uint8_t *rdram, Face &face, float dt, int excitement)
        {
            const FaceData &data = *face.data;
            face.timer += dt;
            const auto level = data.excitementPoses.find(excitement);
            if (data.sung)
                face.setNewPose(singing(rdram, data.eventOffset) ? *data.sung : 0);
            else if (face.timer > data.poseLength && level != data.excitementPoses.end() && !level->second.empty())
            {
                face.timer = 0.0f;
                const std::vector<int> &poses = level->second;
                size_t i = std::uniform_int_distribution<size_t>(0u, poses.size() - 1u)(s_random);
                if (poses[i] == face.pose)
                    i = (i + 1u) % poses.size();
                face.setNewPose(poses[i]);
            }
            if (face.blend != 1.0f)
            {
                face.blend += dt / data.blendTime;
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

        // SimpleBlender::Draw (GH1 0x287f38): the pose, then the next eased
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
            const std::optional<int> excitement = s_polled;
            s_polled.reset();
            std::unordered_map<uint32_t, std::pair<const FaceData *, std::vector<uint32_t>>> morphs;
            for (const uint32_t morph : s_morphs)
            {
                const uint32_t object = load<uint32_t>(rdram, morph);
                const uint32_t name = load<uint32_t>(rdram, object + ::milo::object::kName);
                const uint32_t dir = load<uint32_t>(rdram, object + ::milo::object::kDir);
                if (name == 0u || dir == 0u)
                    continue;
                const char *text = reinterpret_cast<const char *>(getMemPtr(rdram, name));
                for (const auto &[outfit, data] : s_data)
                    if (std::strncmp(text, outfit.c_str(), outfit.size()) == 0 && text[outfit.size()] == '_')
                    {
                        morphs[dir].first = &data;
                        morphs[dir].second.push_back(morph);
                    }
            }
            std::erase_if(s_faces, [&](const auto &face) { return !morphs.count(face.first); });
            if (morphs.empty())
                return;
            const float dt = deltaSeconds(rdram);
            const EntryArgs args(ctx);
            for (const auto &[dir, found] : morphs)
            {
                const auto &[data, list] = found;
                Face &face = s_faces[dir];
                if (excitement)
                {
                    face.data = data;
                    poll(rdram, face, dt, *excitement);
                }
                else
                    face = Face{data};
                for (const uint32_t morph : list)
                    draw(rdram, ctx, runtime, morph, face);
            }
            args.restore(ctx);
        }
    }

    namespace gh1
    {
        // CharFace::PoseNum (GH1 debug 0x2a7620): a pose's frame is its place
        // in poses, after the key.
        bool addFace(const std::string &outfit, const dtb::Node &faceData)
        {
            const dtb::Node *poses = dtb::find(faceData, "poses");
            const dtb::Node *blend = dtb::find(faceData, "blend_time");
            const dtb::Node *excitement = dtb::find(faceData, "excitement_poses");
            const dtb::Node *events = dtb::find(faceData, "event_list");
            if (!poses || !blend || blend->nodes.size() < 2u || !dtb::number(blend->nodes[1]) || (!excitement && !events))
                return false;
            std::map<std::string, int> frames;
            for (size_t i = 1; i < poses->nodes.size(); ++i)
                frames[poses->nodes[i].text] = static_cast<int>(i - 1u);
            FaceData data;
            data.blendTime = *dtb::number(blend->nodes[1]);
            if (const dtb::Node *length = dtb::find(faceData, "pose_length");
                length && length->nodes.size() >= 2u && dtb::number(length->nodes[1]))
                data.poseLength = *dtb::number(length->nodes[1]);
            if (events)
            {
                if (const auto open = frames.find("open"); open != frames.end())
                    data.sung = open->second;
                if (const dtb::Node *offset = dtb::find(faceData, "event_offset");
                    offset && offset->nodes.size() >= 2u && dtb::number(offset->nodes[1]))
                    data.eventOffset = *dtb::number(offset->nodes[1]) / 1000.0f;
            }
            for (const dtb::Node &level : excitement ? excitement->nodes : std::vector<dtb::Node>())
            {
                if (level.type != dtb::kArray || level.nodes.empty() || level.nodes[0].type != dtb::kInt)
                    continue;
                std::vector<int> &list = data.excitementPoses[level.nodes[0].integer];
                for (size_t i = 1; i < level.nodes.size(); ++i)
                    if (const auto f = frames.find(level.nodes[i].text); f != frames.end())
                        list.push_back(f->second);
            }
            s_data[outfit] = std::move(data);
            return true;
        }

        void addSinging(const std::string &song, std::vector<std::pair<float, float>> open)
        {
            s_singing[song] = std::move(open);
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
