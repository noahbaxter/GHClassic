// A player with lefty flip on gets a guitarist who plays left-handed. Retail
// flips the track and nothing else.
//
// The guitarist is the same model, drawn in the mirror of its pose: each bone
// takes what its opposite number is doing, bone_L-hand bone_R-hand's, mirrored
// left for right, and the guitar with everything on it is drawn mirrored
// whole, a left-handed guitar. It plays on as it always has:
// CharServoBone::Poll (0x180a48) rebuilds its transform right-handed each
// frame it turns, as Character::Teleport (0x162b30) does from a waypoint, and
// its walks, hands and hair are all worked out right-handed. So nothing it is
// posed by changes. Between PsRnd's BeginDrawing and EndDrawing the world
// transform of everything under the guitarist stands as it is drawn, and is
// put back after.
//
// A mesh's vertices are carried by up to four bones, each through the bind
// transform the mesh holds for it (PsMesh::DrawShowing 0x3d8974), which is
// the mesh at rest seen from the bone. From those comes where each bone
// rests in the guitarist's own space, the same from whichever mesh it is
// read, and from that what a bone's world must be for its vertices to land
// where its opposite's would, mirrored. Whatever no mesh is carried by, a
// bracelet or an eye, stays on the bone it hangs from.
//
// A camera shot is set before then, by CamShotFrame::Interp (0x2665a0), from
// the world transforms of its keyframes' targets, which it looks at, and of
// their parents (CamShotFrame +0xb0), which a shot that rides on a character
// is placed by (CamShotFrame::BuildTransform, 0x267008). A shot that rides
// on a lefty guitarist is mirrored with them, in their plane and with its
// own x negated, which keeps it a camera: it frames them as it would
// right-handed, the other way round. Any other shot stays where it is and
// looks at them as they are drawn. So Interp reads each transform as it will
// be drawn, and for a shot that is mirrored after, mirrored the once more
// that undoes.

#include "lefty_guitarist.h"

#include "gh1/rig.h"
#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        PS2Runtime::RecompiledFunction s_interp = nullptr;

        constexpr uint32_t kCharacterTransform = 0xe0u; // Character's RndTransformable
        constexpr uint32_t kTransformParent = 0x10u;
        constexpr uint32_t kTransformChildren = 0x18u; // a list's head: a node is next, previous, child
        constexpr uint32_t kOptionsLefty = 0x1cu;      // Options: an int a player (0x10d9f8)
        constexpr uint32_t kFrameParent = 0xb0u;       // CamShotFrame: a ShotTarget
        constexpr uint32_t kShotTargetSize = 0x10u;
        constexpr size_t kTransformSize = 0x40u;
        constexpr char kGuitar[] = "bone_pos_guitar.mesh"; // what the guitar and the hands' places on it hang from

        using Vec3 = std::array<float, 3>;

        Vec3 loadVec(uint8_t *rdram, uint32_t at)
        {
            return {load<float>(rdram, at), load<float>(rdram, at + 4u), load<float>(rdram, at + 8u)};
        }

        void storeVec(uint8_t *rdram, uint32_t at, const Vec3 &v)
        {
            for (uint32_t i = 0; i < 3u; ++i)
                store<float>(rdram, at + 4u * i, v[i]);
        }

        float dot(const Vec3 &a, const Vec3 &b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        // A Transform as the rig's Xfm (gh1/rig.h), where a * b is a and then
        // b: in memory its rows are at +0, +0x10 and +0x20, and its position
        // at +0x30.
        using gh1::Xfm;
        constexpr Xfm kIdentity = {{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}, {}};
        // Left for right in the guitarist's own space, whose x that is.
        constexpr Xfm kFlipped = {{-1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}, {}};

        Xfm readXfm(uint8_t *rdram, uint32_t from)
        {
            Xfm made;
            for (uint32_t i = 0; i < 9u; ++i)
                made.m[i] = load<float>(rdram, from + 0x10u * (i / 3u) + 4u * (i % 3u));
            for (uint32_t i = 0; i < 3u; ++i)
                made.v[i] = load<float>(rdram, from + 0x30u + 4u * i);
            return made;
        }

        void writeXfm(uint8_t *rdram, uint32_t to, const Xfm &x)
        {
            for (uint32_t i = 0; i < 9u; ++i)
                store<float>(rdram, to + 0x10u * (i / 3u) + 4u * (i % 3u), x.m[i]);
            for (uint32_t i = 0; i < 3u; ++i)
                store<float>(rdram, to + 0x30u + 4u * i, x.v[i]);
        }

        struct Plane
        {
            Vec3 normal, point;

            Vec3 turned(const Vec3 &v) const
            {
                const float d = 2.0f * dot(v, normal);
                return {v[0] - d * normal[0], v[1] - d * normal[1], v[2] - d * normal[2]};
            }

            void mirror(uint8_t *rdram, uint32_t transform) const
            {
                for (uint32_t row = 0; row < 3u; ++row)
                    storeVec(rdram, transform + 0x10u * row, turned(loadVec(rdram, transform + 0x10u * row)));
                const Vec3 at = loadVec(rdram, transform + 0x30u);
                const Vec3 from = turned({at[0] - point[0], at[1] - point[1], at[2] - point[2]});
                storeVec(rdram, transform + 0x30u, {from[0] + point[0], from[1] + point[1], from[2] + point[2]});
            }
        };

        // A world transform as it was, to put back.
        struct Kept
        {
            uint32_t world;
            std::array<uint8_t, kTransformSize> bytes;

            Kept(uint8_t *rdram, uint32_t world) : world(world)
            {
                std::memcpy(bytes.data(), getMemPtr(rdram, world), bytes.size());
            }

            void restore(uint8_t *rdram) const
            {
                std::memcpy(getMemPtr(rdram, world), bytes.data(), bytes.size());
            }
        };

        // Calls into the guest, as everything below that takes one does.
        struct Guest
        {
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *runtime;

            uint32_t call(uint32_t function, uint32_t argument) const
            {
                return static_cast<uint32_t>(runtime->callGuestFunction(rdram, ctx, function, {argument}));
            }

            // Clean, so whoever asks next reads what is written there.
            uint32_t world(uint32_t transformable) const
            {
                return call(s_addresses->worldXfm, transformable);
            }
        };

        // A bone some mesh is carried by.
        struct Bone
        {
            Xfm rest;       // in the guitarist's space
            Xfm fromOther;  // its opposite's world to its own as drawn, short of the mirror
            uint32_t other; // its opposite, itself for one in the middle
        };

        // A player's guitarist, from its Poll to its destructor.
        struct Guitarist
        {
            uint32_t transformable = 0u;  // its RndTransformable
            std::vector<uint32_t> meshes; // its skinned ones, which bones is from
            std::unordered_map<uint32_t, Bone> bones;
            uint32_t guitar = 0u;
        };
        std::array<Guitarist, 2> s_guitarists;

        const char *name(uint8_t *rdram, uint32_t transformable)
        {
            const uint32_t text = load<uint32_t>(rdram, load<uint32_t>(rdram, transformable) + ::milo::object::kName);
            return text != 0u ? reinterpret_cast<const char *>(getMemPtr(rdram, text)) : "";
        }

        // 0 or 1 for guitarist0 or guitarist1, the players' own, else -1.
        // A RndTransformable's first word is its Hmx::Object.
        int player(uint8_t *rdram, uint32_t transformable)
        {
            const char *text = name(rdram, transformable);
            if (std::strncmp(text, "guitarist", 9u) != 0 || (text[9] != '0' && text[9] != '1') || text[10] != '\0')
                return -1;
            return text[9] - '0';
        }

        bool lefty(uint8_t *rdram, int who)
        {
            const uint32_t options = load<uint32_t>(rdram, s_addresses->theOptions);
            return settings::get(settings::kTrueLeftyFlip) != 0 && options != 0u &&
                   load<uint32_t>(rdram, options + kOptionsLefty + 4u * static_cast<uint32_t>(who)) != 0u;
        }

        // The plane a lefty player's guitarist is mirrored in.
        std::optional<Plane> plane(const Guest &guest, int who)
        {
            if (s_guitarists[who].transformable == 0u || !lefty(guest.rdram, who))
                return std::nullopt;
            const uint32_t world = guest.world(s_guitarists[who].transformable);
            Plane made = {loadVec(guest.rdram, world), loadVec(guest.rdram, world + 0x30u)};
            const float length = std::sqrt(dot(made.normal, made.normal));
            if (length == 0.0f)
                return std::nullopt;
            for (float &n : made.normal)
                n /= length;
            return made;
        }

        // The player whose guitarist `transformable` is or is inside, or -1.
        int owner(uint8_t *rdram, uint32_t transformable)
        {
            for (int depth = 0; transformable != 0u && depth < 64; ++depth)
            {
                for (int who = 0; who < 2; ++who)
                    if (s_guitarists[who].transformable == transformable)
                        return who;
                transformable = load<uint32_t>(rdram, transformable + kTransformParent);
            }
            return -1;
        }

        // The guitarist and everything under it, each after its parent.
        struct Under
        {
            uint32_t transformable;
            size_t parent;
        };

        std::vector<Under> under(uint8_t *rdram, uint32_t guitarist)
        {
            std::vector<Under> all = {{guitarist, 0u}};
            for (size_t i = 0; i < all.size(); ++i)
            {
                const uint32_t head = all[i].transformable + kTransformChildren;
                for (uint32_t node = load<uint32_t>(rdram, head); node != head; node = load<uint32_t>(rdram, node))
                    all.push_back({load<uint32_t>(rdram, node + 8u), i});
            }
            return all;
        }

        // bone_L-hand.mesh for bone_R-hand.mesh, bone_lowerlip-R2.mesh for
        // bone_lowerlip-L2.mesh, and a name with no side in it as it is.
        std::string opposite(std::string text)
        {
            for (size_t i = 1; i < text.size(); ++i)
            {
                const char before = text[i - 1], after = i + 1 < text.size() ? text[i + 1] : '.';
                if ((text[i] == 'L' || text[i] == 'R') && (before == '_' || before == '-') &&
                    (after == '-' || after == '.' || (after >= '0' && after <= '9')))
                {
                    text[i] = text[i] == 'L' ? 'R' : 'L';
                    break;
                }
            }
            return text;
        }

        // Where each bone rests, from the meshes: a mesh at rest is where its
        // parent's rest puts it, and a bone's bind is that seen from the bone.
        // So those hung from nothing that moves are read first, and the ones
        // hung from a bone, hair from the head, once that bone's rest is known.
        void learn(uint8_t *rdram, Guitarist &guitarist)
        {
            guitarist.bones.clear();
            guitarist.guitar = 0u;
            const auto boneOf = [&](uint32_t bones, uint32_t b) {
                return load<uint32_t>(rdram, bones + ::milo::mesh::kBoneObject + b * ::milo::mesh::kBoneObjectStride);
            };
            std::unordered_map<uint32_t, bool> carries; // every bone of every mesh
            for (const uint32_t mesh : guitarist.meshes)
            {
                const uint32_t bones = load<uint32_t>(rdram, mesh + ::milo::mesh::kBones);
                for (uint32_t b = 0; bones != 0u && b < ::milo::mesh::kBoneCount; ++b)
                    carries[boneOf(bones, b)] = true;
            }
            // Null where one a mesh rests on is not known yet.
            const auto rest = [&](uint32_t transformable, bool known) -> std::optional<Xfm> {
                Xfm made = kIdentity;
                for (int depth = 0; transformable != guitarist.transformable; ++depth)
                {
                    if (transformable == 0u || depth == 64)
                        return std::nullopt;
                    if (const auto bone = guitarist.bones.find(transformable); bone != guitarist.bones.end())
                        return made * bone->second.rest;
                    if (!known && carries.count(transformable) != 0u)
                        return std::nullopt;
                    made = made * readXfm(rdram, transformable + ::milo::transformable::kLocal);
                    transformable = load<uint32_t>(rdram, transformable + kTransformParent);
                }
                return made;
            };
            for (int pass = 0; pass < 3; ++pass)
                for (const uint32_t mesh : guitarist.meshes)
                {
                    const uint32_t bones = load<uint32_t>(rdram, mesh + ::milo::mesh::kBones);
                    const std::optional<Xfm> meshRest = rest(mesh + ::milo::mesh::kTransform, pass == 2);
                    if (bones == 0u || !meshRest)
                        continue;
                    for (uint32_t b = 0; b < ::milo::mesh::kBoneCount; ++b)
                    {
                        const uint32_t bone = boneOf(bones, b);
                        if (bone == 0u || guitarist.bones.count(bone) != 0u)
                            continue;
                        const Xfm bind =
                            readXfm(rdram, bones + ::milo::mesh::kBoneBind + b * ::milo::mesh::kBoneBindStride);
                        guitarist.bones[bone] = {inverse(bind) * *meshRest, {}, bone};
                    }
                }
            std::unordered_map<std::string, uint32_t> named;
            for (const Under &one : under(rdram, guitarist.transformable))
            {
                const char *text = name(rdram, one.transformable);
                named[text] = one.transformable;
                if (std::strcmp(text, kGuitar) == 0)
                    guitarist.guitar = one.transformable;
            }
            for (auto &[transformable, bone] : guitarist.bones)
            {
                const auto other = named.find(opposite(name(rdram, transformable)));
                if (other != named.end() && guitarist.bones.count(other->second) != 0u)
                    bone.other = other->second;
            }
            for (auto &[transformable, bone] : guitarist.bones)
                bone.fromOther = bone.rest * kFlipped * inverse(guitarist.bones[bone.other].rest);
        }

        // Everything under a lefty player's guitarist as it is drawn: its
        // world transform's address and what stands there then. Each is left
        // clean.
        struct Drawn
        {
            uint32_t transformable, world;
            Xfm as;
        };

        std::vector<Drawn> drawn(const Guest &guest, int who)
        {
            Guitarist &guitarist = s_guitarists[who];
            const std::vector<Under> all = under(guest.rdram, guitarist.transformable);
            // The skinned meshes among them, bones being meshes too: learned
            // from before the first is drawn, and again should they change.
            std::vector<uint32_t> meshes;
            for (const Under &one : all)
            {
                const std::string_view text = name(guest.rdram, one.transformable);
                const uint32_t mesh = one.transformable - ::milo::mesh::kTransform;
                if (text.ends_with(".mesh") && load<uint32_t>(guest.rdram, mesh + ::milo::mesh::kBones) != 0u)
                    meshes.push_back(mesh);
            }
            if (meshes != guitarist.meshes)
            {
                guitarist.meshes = std::move(meshes);
                learn(guest.rdram, guitarist);
            }
            std::vector<Drawn> made(all.size());
            std::vector<Xfm> is(all.size());
            std::unordered_map<uint32_t, size_t> index;
            // All of them clean before any is read, since a child's is made
            // from its parent's.
            for (size_t i = 0; i < all.size(); ++i)
            {
                made[i] = {all[i].transformable, guest.world(all[i].transformable), {}};
                index[all[i].transformable] = i;
            }
            for (size_t i = 0; i < all.size(); ++i)
                is[i] = readXfm(guest.rdram, made[i].world);
            const Xfm mirror = inverse(is[0]) * kFlipped * is[0];
            std::vector<bool> onGuitar(all.size(), false);
            made[0].as = is[0];
            for (size_t i = 1; i < all.size(); ++i)
            {
                const size_t parent = all[i].parent;
                onGuitar[i] = onGuitar[parent] || all[i].transformable == guitarist.guitar;
                const auto bone = guitarist.bones.find(all[i].transformable);
                const auto other = bone != guitarist.bones.end() ? index.find(bone->second.other) : index.end();
                if (onGuitar[i])
                    made[i].as = is[i] * mirror;
                else if (other != index.end())
                    made[i].as = bone->second.fromOther * is[other->second] * mirror;
                else
                    made[i].as = is[i] * inverse(is[parent]) * made[parent].as;
            }
            return made;
        }

        struct PollTag;
        void onPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t transformable = GPR_U32(ctx, 4) + kCharacterTransform;
            if (const int who = player(rdram, transformable); who >= 0 && s_guitarists[who].transformable != transformable)
            {
                s_guitarists[who] = {};
                s_guitarists[who].transformable = transformable;
            }
        }

        struct DtorTag;
        void onDtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            for (Guitarist &guitarist : s_guitarists)
                if (guitarist.transformable == GPR_U32(ctx, 4) + kCharacterTransform)
                    guitarist = {};
        }

        std::vector<Kept> s_drawn;

        void putBack(uint8_t *rdram)
        {
            for (const Kept &one : s_drawn)
                one.restore(rdram);
            s_drawn.clear();
        }

        struct BeginTag;
        void onBeginDrawing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            putBack(rdram);
            const EntryArgs args(ctx);
            const Guest guest = {rdram, ctx, runtime};
            for (int who = 0; who < 2; ++who)
            {
                if (s_guitarists[who].transformable == 0u || !lefty(rdram, who))
                    continue;
                for (const Drawn &one : drawn(guest, who))
                {
                    s_drawn.emplace_back(rdram, one.world);
                    writeXfm(rdram, one.world, one.as);
                }
            }
            args.restore(ctx);
        }

        struct EndTag;
        void onEndDrawing(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            putBack(rdram);
        }

        // CamShotFrame::Interp(shot, next, t, blend, cam)
        void interp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const EntryArgs args(ctx);
            const Guest guest = {rdram, ctx, runtime};
            const std::optional<Plane> planes[] = {plane(guest, 0), plane(guest, 1)};
            if (!planes[0] && !planes[1])
            {
                args.restore(ctx);
                return s_interp(rdram, ctx, runtime);
            }
            const uint32_t keyframes[] = {args.a[0], args.a[2]}, camera = args.a[3];
            int rides = -1;             // the lefty player whose guitarist the shot rides on
            std::vector<uint32_t> read; // what Interp takes a world transform of
            const auto add = [&](uint32_t shotTarget) {
                const uint32_t transformable = guest.call(s_addresses->shotTargetResolve, shotTarget);
                if (transformable != 0u && std::find(read.begin(), read.end(), transformable) == read.end())
                    read.push_back(transformable);
                return transformable;
            };
            for (const uint32_t keyframe : keyframes)
            {
                const uint32_t parent = add(keyframe + kFrameParent);
                if (const int who = owner(rdram, parent); rides < 0 && who >= 0 && planes[who])
                    rides = who;
                for (uint32_t target = load<uint32_t>(rdram, keyframe + ::milo::camShotFrame::kTargets);
                     target < load<uint32_t>(rdram, keyframe + ::milo::camShotFrame::kTargets + 4u);
                     target += kShotTargetSize)
                    add(target);
            }
            // A shot that rides is worked out from its guitarist as it is
            // posed, whose mirror is where it is drawn.
            std::optional<std::vector<Drawn>> as[2];
            std::vector<Kept> kept;
            for (const uint32_t transformable : read)
            {
                const int who = owner(rdram, transformable);
                const bool drawnMirrored = who >= 0 && planes[who];
                if (who == rides && drawnMirrored)
                    continue;
                const uint32_t world = guest.world(transformable);
                kept.emplace_back(rdram, world);
                if (drawnMirrored)
                {
                    if (!as[who])
                        as[who] = drawn(guest, who);
                    for (const Drawn &one : *as[who])
                        if (one.transformable == transformable)
                            writeXfm(rdram, world, one.as);
                }
                if (rides >= 0)
                    planes[rides]->mirror(rdram, world);
            }
            // The camera Interp blends from is the last frame's, mirrored.
            const auto mirrorCamera = [&] {
                if (rides < 0)
                    return;
                const uint32_t local = camera + ::milo::transformable::kLocal;
                planes[rides]->mirror(rdram, local);
                const Vec3 x = loadVec(rdram, local);
                storeVec(rdram, local, {-x[0], -x[1], -x[2]});
                guest.call(s_addresses->setDirty, camera);
            };
            mirrorCamera();
            // callGuestFunction loads the integer arguments alone, and the
            // calls above may have left anything in t and blend's registers.
            args.restore(ctx);
            runtime->callGuestFunction(rdram, ctx, s_addresses->camShotFrameInterp,
                                       {args.a[0], args.a[1], args.a[2], args.a[3]}, s_interp);
            for (const Kept &one : kept)
                one.restore(rdram);
            mirrorCamera();
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void installLeftyGuitarist(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<PollTag>::install(runtime, addresses.guitarCharacterPoll, onPoll);
        EntryHook<DtorTag>::install(runtime, addresses.guitarCharacterDtor, onDtor);
        EntryHook<BeginTag>::install(runtime, addresses.psRndBeginDrawing, onBeginDrawing);
        EntryHook<EndTag>::install(runtime, addresses.psRndEndDrawing, onEndDrawing);
        s_interp = runtime.lookupFunction(addresses.camShotFrameInterp);
        runtime.replaceFunction(addresses.camShotFrameInterp, &interp);
    }
}
