#include "gh2x/classes.h"

#include "formats/bitmap.h"
#include "gh2x/focus.h"
#include "gh2x/stream.h"

#include <functional>
#include <map>
#include <set>

namespace gh2::gh2x
{
    namespace
    {
        // Hmx::Object, which every class starts or ends with: revision 1 on
        // the 360 with a note string last, 0 on the PS2 without.
        struct Object
        {
            std::string type;
            Bytes props;
        };

        // A DataArray of type properties: a node count, four header bytes,
        // and each node's type and value.
        void skipArray(Reader &r);

        void skipNode(Reader &r)
        {
            switch (r.u32())
            {
            case 0: case 1: case 6: case 7: case 8: case 9:
                r.skip(4u);
                break;
            case 2: case 3: case 4: case 5: case 18: case 32: case 33: case 34: case 35: case 36:
                r.str(65536u); // variable, function, object, symbol, string, directives
                break;
            case 16: case 17: case 19:
                skipArray(r);
                break;
            default:
                r.ok = false;
            }
        }

        void skipArray(Reader &r)
        {
            const uint16_t nodes = r.u16();
            r.skip(4u);
            for (uint16_t i = 0; i < nodes && r.ok; ++i)
                skipNode(r);
        }

        Bytes props(Reader &r)
        {
            const size_t from = r.o;
            if (r.flag())
                skipArray(r);
            return r.since(from);
        }

        Object object(Reader &r)
        {
            r.rev(1u);
            Object o;
            o.type = r.str();
            o.props = props(r);
            r.str(); // the note
            return o;
        }

        Writer &put(Writer &w, const Object &o) { return w.u32(0u).str(o.type).raw(o.props); }

        // RndTransformable, revision 9 on both: local and world transforms,
        // constraint, target, preserve scale, parent.
        Bytes trans(Reader &r)
        {
            const size_t from = r.o;
            r.rev(9u);
            r.skip(96u);
            r.u32();
            r.str();
            r.flag();
            r.str();
            return r.since(from);
        }

        // RndDrawable, revision 3: showing, sphere, draw order.
        Bytes draw(Reader &r)
        {
            const size_t from = r.o;
            r.rev(3u);
            r.flag();
            r.skip(20u);
            return r.since(from);
        }

        // CharWeightable, revision 2 on both: weight and its owner.
        Bytes weightable(Reader &r)
        {
            const size_t from = r.o;
            r.rev(2u);
            r.skip(4u);
            r.str();
            return r.since(from);
        }

        std::optional<Bytes> finish(const Reader &r, Writer &w)
        {
            return r.done() ? std::optional(std::move(w.out)) : std::nullopt;
        }

        // Only the Object block differs (Trans 6,167 of 6,275 the PS2's
        // length, CharClipGroup 328 of 329 exact).
        std::optional<Bytes> objectAt4(const Bytes &body)
        {
            if (body.size() < 4u)
                return std::nullopt;
            Reader r{body, 4u};
            const Object o = object(r);
            if (!r.ok)
                return std::nullopt;
            Writer w;
            w.raw(body.data(), 4u);
            put(w, o).raw(body.data() + r.o, body.size() - r.o);
            return std::move(w.out);
        }

        // Rev 28 on both discs; the 360's has no strips. The PS2's goes on
        // with one block a face group: strip count and, unless none, index
        // total, each strip's end and the indices. One strip a triangle, as
        // GH1's are made (gh1/scene.cpp).
        std::optional<Bytes> mesh(const Bytes &body)
        {
            Reader r{body};
            r.rev(28u);
            const Object o = object(r);
            const size_t from = r.o;
            trans(r);
            draw(r);
            r.str(); // mat
            r.str(); // geometry owner
            r.u32(); // mutable
            r.u32(); // volume
            if (r.u8() != 0u) // a BSP tree
                return std::nullopt;
            const uint32_t verts = r.count(65535u);
            r.skip(48u * verts);
            const uint32_t faces = r.count(200000u);
            const size_t facesAt = r.o;
            r.skip(6u * faces);
            const Bytes groups = r.raw(r.count(200000u));
            // One drawing its owner's geometry has no faces but the owner's
            // group sizes, and on the PS2 no strips.
            uint32_t grouped = 0u;
            for (const uint8_t g : groups)
                grouped += g;
            if (faces != 0u && grouped != faces)
                return std::nullopt;
            if (!r.str().empty())
            {
                for (int i = 0; i < 3; ++i)
                    r.str();
                r.skip(192u); // bind transforms
            }
            if (!r.done())
                return std::nullopt;
            Writer w;
            put(w.u32(28u), o).raw(body.data() + from, r.o - from);
            size_t at = facesAt;
            for (const uint8_t g : faces ? groups : Bytes{})
            {
                w.u32(g);
                if (g != 0u)
                {
                    w.u32(3u * g);
                    for (uint32_t i = 1; i <= g; ++i)
                        w.u32(3u * i);
                    w.raw(body.data() + at, 6u * g);
                }
                at += 6u * g;
            }
            return std::move(w.out);
        }

        // Light 9 is 6 with six fields no light uses.
        std::optional<Bytes> light(const Bytes &body)
        {
            Reader r{body};
            r.rev(9u);
            const Object o = object(r);
            const size_t from = r.o;
            trans(r);
            r.skip(16u + 4u); // colour, range
            r.u32();          // type
            r.flag();
            r.flag();
            const Bytes mid = r.since(from);
            r.skip(16u);
            r.str();
            r.str();
            Writer w;
            put(w.u32(6u), o).raw(mid);
            return finish(r, w);
        }

        // Rev 1 has a zero word after the revision (72 of 72 exact).
        std::optional<Bytes> clipFilter(const Bytes &body)
        {
            static const uint8_t kRev1[8] = {1, 0, 0, 0, 0, 0, 0, 0};
            if (body.size() < 8u || std::memcmp(body.data(), kRev1, 8u) != 0)
                return std::nullopt;
            Bytes out(4u, 0u);
            out.insert(out.end(), body.begin() + 8, body.end());
            return out;
        }

        std::optional<Bytes> driverMidi(const Bytes &body)
        {
            Reader r{body};
            r.rev(3u);
            r.rev(5u);
            const Object o = object(r);
            const Bytes weight = weightable(r);
            const std::string bones = r.str(), clips = r.str();
            const bool realign = r.flag();
            r.flag(); // apply
            r.str();  // test clip
            if (!r.ok)
                return std::nullopt;
            Writer w;
            put(w.u32(3u).u32(3u), o).raw(weight).str(bones).str(clips).u8(realign);
            return std::move(w.raw(body.data() + r.o, body.size() - r.o).out);
        }

        // CamShot 27 -> 20 (the PS2 debug build's CamShot::Save, RB3's
        // gates). A keyframe's depth of field is 360 only, and the shot has
        // the PS2's off (on in all 440 of the 360's shots, off in 416 of the
        // PS2's 441), so its three blur fields are read by nothing in the
        // game and carry the 360's to the host (gh2x/focus.h). A shot with
        // the 360's off gets the PS2's usual far blur and focal blur. Lost:
        // zoom fov and the shot's post process; the focal target goes to
        // camShotFocus.
        constexpr float kFarBlur = 2.0f;
        constexpr float kMaxFocalBlur = 0.9f;
        // The category of a shot that only holds focal targets, which the
        // game never asks for a shot of.
        const char *const kFocusCategory = "x360_focus";

        Bytes withoutProps(const Bytes &raw, const std::set<std::string> &names);

        std::optional<Bytes> camShot(const Bytes &body)
        {
            Reader r{body};
            r.rev(27u);
            Object o = object(r);
            o.props = withoutProps(o.props, {"postprocess"});
            Writer w;
            put(w.u32(20u), o);
            r.rev(4u);
            w.u32(4u).raw(r.raw(8u)); // frame, rate
            const uint32_t keys = r.count(1000u);
            w.u32(keys);
            auto target = [&](Reader &r, Writer *w)
            {
                r.rev(27u);
                const std::string entity = r.str(), subpart = r.str();
                if (w)
                    w->u32(20u).str(entity).str(subpart);
            };
            std::vector<size_t> blurs; // where each keyframe's far blur is written
            for (uint32_t k = 0; k < keys && r.ok; ++k)
            {
                w.raw(r.raw(16u + 48u + 8u)); // duration, blend, ease, fov, transform, screen offset
                const float blurDepth = r.f32(), maxBlur = r.f32(), focusBlur = r.f32();
                w.f32(blurDepth);
                blurs.push_back(w.out.size());
                w.f32(focusBlur).f32(kShotBlurMark + maxBlur);
                const uint32_t targets = r.count(64u);
                w.u32(targets);
                for (uint32_t t = 0; t < targets && r.ok; ++t)
                    target(r, &w);
                target(r, nullptr); // focal target
                target(r, &w);      // parent
                w.u8(r.flag()).raw(r.raw(16u)); // use parent rotation, shake
                r.skip(4u);                     // zoom fov
            }
            w.u8(r.flag()).raw(r.raw(12u)); // looping, path ease, near, far
            if (!r.flag())
                for (const size_t at : blurs)
                {
                    const float usual[2] = {kFarBlur, kMaxFocalBlur};
                    std::memcpy(w.out.data() + at, usual, sizeof(usual));
                }
            w.u8(0u).raw(r.raw(8u)); // depth of field, filter, clamp height
            w.str(r.str()).raw(r.raw(4u)).str(r.str()).raw(r.raw(4u)); // path, fade time, category, weight
            const uint32_t crowd = r.count(100000u);
            w.u32(crowd).raw(r.raw(8u * crowd)).raw(r.raw(4u)); // crowd characters, stamp
            w.strs(r.strs()).str(r.str()).str(r.str()); // hide list, crowd, glow spot
            return finish(r, w);
        }

        // LightPreset 9 -> 8: a spotlight entry ends with a word 9 dropped,
        // its flare step count; 0 leaves the flare alone.
        std::optional<Bytes> lightPreset(const Bytes &body)
        {
            Reader r{body};
            r.rev(9u);
            const Object o = object(r);
            Writer w;
            put(w.u32(8u), o);
            r.rev(4u);
            w.u32(4u).raw(r.raw(8u));
            const uint32_t keys = r.count(1000u);
            w.u32(keys);
            std::vector<uint32_t> sizes;
            for (uint32_t k = 0; k < keys && r.ok; ++k)
            {
                w.raw(r.raw(8u)); // duration, fade out
                const uint32_t spots = r.count(1000u);
                w.u32(spots);
                for (uint32_t i = 0; i < spots && r.ok; ++i)
                {
                    w.raw(r.raw(4u + 16u + 16u)).str(r.str()).str(r.str()).u8(r.flag()).u32(0u);
                }
                const uint32_t envs = r.count(1000u);
                w.u32(envs);
                for (uint32_t i = 0; i < envs && r.ok; ++i)
                    w.raw(r.raw(16u)).u8(r.flag()).raw(r.raw(8u + 16u));
                const uint32_t lights = r.count(1000u);
                w.u32(lights).raw(r.raw(52u * lights));
                w.str(r.str());
                sizes.insert(sizes.end(), {spots, envs, lights});
            }
            const std::vector<std::string> spots = r.strs(), envs = r.strs(), lights = r.strs();
            w.strs(spots).strs(envs).strs(lights);
            w.u8(r.flag()).str(r.str()).strs(r.strs()).str(r.str()); // looping, category, adjectives, trigger
            w.raw(r.raw(12u)).u8(r.flag()).u8(r.flag()); // fade in, two words, manual, locked
            for (size_t i = 0; i < sizes.size(); i += 3u)
                if (sizes[i] != spots.size() || sizes[i + 1u] != envs.size() || sizes[i + 2u] != lights.size())
                    return std::nullopt;
            return finish(r, w);
        }

        // EventTrigger 10 -> 8 (the retail EventTrigger::Load, 0x27ca00). A
        // PS2 trigger listens for one event, so a 360 one listening for
        // several keeps the first. Lost: each anim's rate, range and period,
        // and a hide's delay.
        std::optional<Bytes> eventTrigger(const Bytes &body)
        {
            Reader r{body};
            r.rev(10u);
            const Object o = object(r);
            Writer w;
            put(w.u32(8u), o);
            const std::vector<std::string> events = r.strs();
            w.str(events.empty() ? std::string() : events[0]);
            const uint32_t anims = r.count(1000u);
            w.u32(anims);
            for (uint32_t i = 0; i < anims && r.ok; ++i)
            {
                w.str(r.str()).raw(r.raw(4u)).u8(r.flag()).raw(r.raw(4u)); // anim, blend, wait, delay
                r.flag();
                r.skip(16u);
                r.str();
                r.skip(4u);
            }
            w.strs(r.strs()).strs(r.strs()); // sounds, shows
            const uint32_t hides = r.count(1000u);
            w.u32(hides);
            for (uint32_t i = 0; i < hides && r.ok; ++i)
            {
                w.str(r.str());
                r.skip(4u);
            }
            w.strs(r.strs()).strs(r.strs()).strs(r.strs()).str(r.str()); // enable, disable, wait for, next
            const uint32_t calls = r.count(1000u);
            w.u32(calls);
            for (uint32_t i = 0; i < calls && r.ok; ++i)
                w.str(r.str()).str(r.str());
            return finish(r, w);
        }

        // ParticleSys 30 -> 27 (the retail RndParticleSys::Load, 0x1ce8e8).
        // Lost: RndPollable's Object block, particle rotation, frame drive
        // and fast forward.
        std::optional<Bytes> particleSys(const Bytes &body)
        {
            Reader r{body};
            r.rev(30u);
            const Object o = object(r);
            object(r); // RndPollable's
            Writer w;
            put(w.u32(27u), o);
            r.rev(4u);
            w.u32(4u).raw(r.raw(8u)).raw(trans(r)).raw(draw(r));
            w.raw(r.raw(36u * 4u)); // life to end colour, 36 floats
            w.str(r.str()).raw(r.raw(12u)).str(r.str()); // bounce, force, mat
            w.raw(r.raw(4u + 12u + 32u + 4u + 16u)).u8(r.flag()); // type to bubble size, bubble
            r.flag();
            r.skip(16u); // rotate
            w.raw(r.raw(4u)).str(r.str()).str(r.str()); // relative motion, parent, mesh
            r.flag();
            r.flag(); // frame drive, fast forward
            const bool preserve = r.flag();
            w.u8(preserve);
            if (preserve)
            {
                const uint32_t n = r.count(100000u);
                w.u32(n).raw(r.raw(32u * n));
            }
            return finish(r, w);
        }

    }

    // The fields particleSys drops, read as it reads them: rotate, then rpm
    // low and high, rpm drag and drag (RB3's names for them; drag is 0 on
    // every system and rpm drag on all but one).
    std::optional<Spin> particleSpin(const Bytes &body)
    {
        Reader r{body};
        r.rev(30u);
        object(r);
        object(r);
        r.rev(4u);
        r.raw(8u);
        trans(r);
        draw(r);
        r.skip(36u * 4u);
        r.str();
        r.skip(12u);
        Spin spin;
        spin.mat = r.str();
        r.skip(4u + 12u + 32u + 4u + 16u);
        r.flag();
        const bool rotate = r.flag();
        spin.rpm[0] = r.f32();
        spin.rpm[1] = r.f32();
        if (!r.ok || !rotate)
            return std::nullopt;
        return spin;
    }

    namespace
    {
        // WorldCrowd 6 on both, its Object block last.
        std::optional<Bytes> worldCrowd(const Bytes &body)
        {
            Reader r{body};
            r.rev(6u);
            draw(r);
            r.str();
            r.u32();
            r.flag();
            const uint32_t chars = r.count(64u);
            for (uint32_t i = 0; i < chars && r.ok; ++i)
            {
                r.str();
                r.skip(12u);
            }
            for (uint32_t i = 0; i < chars && r.ok; ++i)
                r.skip(48u * r.count(100000u));
            r.skip(4u);
            r.flag();
            const size_t at = r.o;
            const Object o = object(r);
            Writer w;
            w.raw(body.data(), at);
            put(w, o);
            return finish(r, w);
        }

        std::optional<Bytes> ikHand(const Bytes &body)
        {
            Reader r{body};
            r.rev(5u);
            const Object o = object(r);
            Writer w;
            put(w.u32(2u), o).raw(weightable(r)).str(r.str()); // hand
            r.str(); // finger
            const std::vector<std::string> targets = r.strs();
            w.str(targets.empty() ? std::string() : targets[0]);
            w.u8(r.flag()).u8(r.flag()).u8(r.flag()); // orientation, stretch, scalable
            r.flag(); // move elbow
            return finish(r, w);
        }

        std::optional<Bytes> driver(const Bytes &body)
        {
            Reader r{body};
            r.rev(5u);
            const Object o = object(r);
            Writer w;
            put(w.u32(3u), o).raw(weightable(r)).str(r.str()).str(r.str()).u8(r.flag()); // bones, clips, realign
            r.flag(); // apply
            r.str();  // test clip
            return finish(r, w);
        }

        std::optional<Bytes> eyes(const Bytes &body)
        {
            Reader r{body};
            r.rev(4u);
            const Object o = object(r);
            Writer w;
            put(w.u32(3u), o).strs(r.strs()).str(r.str()); // eyes, head
            const uint32_t interests = r.count(64u);
            for (uint32_t i = 0; i < interests && r.ok; ++i)
            {
                r.str();
                r.u32();
            }
            return finish(r, w);
        }

        std::optional<Bytes> lookAt(const Bytes &body)
        {
            Reader r{body};
            r.rev(3u);
            const Object o = object(r);
            Writer w;
            put(w.u32(2u), o).raw(weightable(r)).str(r.str()).str(r.str()).str(r.str()).raw(r.raw(32u));
            r.flag(); // allow roll
            return finish(r, w);
        }

        std::optional<Bytes> weightSetter(const Bytes &body)
        {
            Reader r{body};
            r.rev(3u);
            const Object o = object(r);
            Writer w;
            put(w.u32(2u), o).raw(weightable(r)).str(r.str()).u32(r.u32()); // driver, flags
            r.flag(); // invert
            return finish(r, w);
        }

        // CharClipSamples 11 -> 10 (the retail CharClipSamples::Load,
        // 0x16b608): 11 stores a weight after each bone name of a sample
        // set's header.
        std::optional<Bytes> clipSamples(const Bytes &body)
        {
            Reader r{body};
            r.rev(11u);
            r.rev(5u);
            const Object o = object(r);
            Writer w;
            put(w.u32(10u).u32(5u), o);
            const size_t from = r.o;
            r.skip(28u);
            r.flag();
            const uint32_t nodes = r.count(4096u);
            for (uint32_t i = 0; i < nodes && r.ok; ++i)
            {
                r.str();
                r.skip(8u * r.count(4096u));
            }
            r.str(65536u);
            r.str(65536u);
            const uint32_t events = r.count(4096u);
            for (uint32_t i = 0; i < events && r.ok; ++i)
            {
                r.skip(4u);
                r.str(65536u);
            }
            w.raw(r.since(from));
            size_t data = 0u;
            for (int set = 0; set < 3 && r.ok; ++set)
            {
                const uint32_t bones = r.count(4096u);
                w.u32(bones);
                for (uint32_t i = 0; i < bones && r.ok; ++i)
                {
                    w.str(r.str());
                    r.skip(4u);
                }
                uint32_t c[10];
                for (uint32_t &n : c)
                    w.u32(n = r.u32());
                const uint32_t compression = r.u32(), samples = r.u32();
                w.u32(compression).u32(samples);
                if (compression > 1u || !(c[0] <= c[1] && c[1] <= c[2] && c[2] <= c[3] && c[3] <= c[6]) ||
                    c[6] != bones)
                    return std::nullopt;
                const bool big = compression == 0u;
                data += size_t{samples} *
                        (12u * c[2] + (big ? 16u : 8u) * (c[3] - c[2]) + (big ? 4u : 2u) * (c[6] - c[3]));
            }
            w.raw(r.raw(data));
            return finish(r, w);
        }

        // CharBone 3 -> 2: 3 adds a word at the end, 1.0 on almost every bone.
        std::optional<Bytes> bone(const Bytes &body)
        {
            Reader r{body};
            r.rev(3u);
            const Object o = object(r);
            Writer w;
            put(w.u32(2u), o).raw(trans(r)).u8(r.flag()).u8(r.flag()).raw(r.raw(8u)); // position, scale, rotation, word
            r.skip(4u);
            return finish(r, w);
        }

        // Sfx 5 -> 4 (the retail Sfx::Load, 0x224e58): 5 adds a reverb send.
        std::optional<Bytes> sfx(const Bytes &body)
        {
            Reader r{body};
            r.rev(5u);
            const Object o = object(r);
            Writer w;
            put(w.u32(4u), o);
            const uint32_t maps = r.count(256u);
            w.u32(maps);
            for (uint32_t i = 0; i < maps && r.ok; ++i)
            {
                w.str(r.str()).raw(r.raw(16u)); // sample, volume, pan, transpose, a word
                r.rev(1u);
                w.u32(1u).raw(r.raw(32u)); // ADSR
            }
            r.str();
            r.skip(4u);
            return finish(r, w);
        }

        // Flare 4: the retail RndFlare::Load (0x1f8c78) takes revision 4.
        std::optional<Bytes> flare(const Bytes &body)
        {
            Reader r{body};
            r.rev(4u);
            object(r);
            trans(r);
            draw(r);
            r.str();
            r.skip(20u);
            return r.done() ? objectAt4(body) : std::nullopt;
        }

        // A directory's own object, which serves as a scene's root and as a
        // proxy's body: its class's part around its parents',
        // Character(RndDir(ObjectDir)). ObjectDir 17 moves the Object block
        // from the start to after the current camera; RndDir 9 drops the
        // three test events (the editor's "fire test" buttons, read at rev 4
        // and up by RndDir::PostLoad 0x1b4ca8), filled with what 314 of the
        // PS2's 462 hold; Character 10 adds a CharacterTest block, WorldDir
        // 12 eight bytes, zero in all 24 files.
        const std::map<std::string, std::string> kDirBase = {
            {"Character", "Character"}, {"BandCharacter", "Character"}, {"RndDir", "RndDir"},
            {"ObjectDir", "ObjectDir"}, {"PanelDir", "RndDir"},         {"WorldDir", "RndDir"}};
        constexpr const char *kTestEvent = "start";

        std::optional<Bytes> dir(const std::string &className, const Bytes &body, const std::string *hud)
        {
            const std::string &base = kDirBase.at(className);
            const bool world = className == "WorldDir", panel = world || className == "PanelDir";
            Reader r{body};
            Writer w;
            if (world)
            {
                r.rev(12u);
                const Bytes head = r.raw(8u); // a word, a float
                const std::string own = r.str();
                w.u32(11u).raw(head).str(hud ? *hud : own);
            }
            // Each part's revision, the 360's then the PS2's.
            const auto part = [&](bool has, uint32_t xbox, uint32_t ps2)
            {
                if (has)
                {
                    r.rev(xbox);
                    w.u32(ps2);
                }
            };
            part(panel, 2u, 2u);
            part(className == "BandCharacter", 1u, 1u);
            part(base == "Character", 10u, 9u);
            part(base != "ObjectDir", 9u, 8u);
            r.rev(17u);
            const size_t from = r.o;
            r.skip(52u * r.count(64u)); // viewports
            r.u32();
            const bool proxy = !r.str().empty();
            r.strs();
            if (r.u32() != 0u) // an inlined proxy: its scene follows in the stream
                return std::nullopt;
            r.str();
            r.str();
            const Bytes head = r.since(from);
            const Object o = object(r);
            put(w.u32(16u), o).raw(head);
            if (base != "ObjectDir")
            {
                const size_t at = r.o;
                r.rev(4u);
                r.skip(8u);
                draw(r);
                trans(r);
                r.str(); // environ
                w.raw(r.since(at)).str(kTestEvent).str(kTestEvent).str(kTestEvent);
            }
            if (base == "Character" && !proxy)
            {
                const size_t at = r.o;
                const uint32_t lods = r.count(16u);
                for (uint32_t i = 0; i < lods && r.ok; ++i)
                {
                    r.skip(4u);
                    r.str();
                }
                r.str(); // shadow
                r.flag();
                r.str(); // sphere base
                w.raw(r.since(at));
                // CharacterTest, revision 6, its driver's name first.
                const size_t test = r.o;
                r.rev(6u);
                r.str();
                r.o = test;
                if (!r.ok)
                    return std::nullopt;
                r.o = body.size();
            }
            if (panel)
                w.str(r.str()).str(r.str()); // camera, enter event
            if (world)
            {
                w.raw(r.raw(48u));
                r.skip(8u);
            }
            return finish(r, w);
        }

        // CharClipSet 16 -> 14, a clip scene's root: ObjectDir, a float, a
        // word (16 adds another), an entry per clip (name, flags, size), and
        // a tail. The count is not stored: it is found as the one the tail
        // fits after. The size word is 196 less on the PS2 for each of the
        // 1,441 clips whose bodies convert to their PS2 twins exactly; what
        // it measures is not known.
        constexpr uint32_t kClipSizeDelta = 196u;

        std::optional<Bytes> clipSet(const Bytes &body)
        {
            Reader r{body};
            r.rev(16u);
            r.rev(17u);
            const size_t from = r.o;
            r.skip(52u * r.count(64u));
            r.u32();
            r.str();
            r.strs();
            r.u32();
            r.str();
            r.str();
            const Bytes head = r.since(from);
            const Object o = object(r);
            const Bytes values = r.raw(8u);
            r.u32();
            if (!r.ok)
                return std::nullopt;
            const size_t start = r.o;
            for (uint32_t n = 0; n < 4096u; ++n)
            {
                Reader t{body, start};
                Writer w;
                put(w.u32(14u).u32(16u), o).raw(head).raw(values);
                for (uint32_t i = 0; i < n && t.ok; ++i)
                {
                    w.str(t.str()).u32(t.u32());
                    const uint32_t size = t.u32();
                    t.ok = t.ok && size >= kClipSizeDelta;
                    w.u32(size - kClipSizeDelta);
                }
                w.u8(t.flag()).strs(t.strs()).strs(t.strs()).u8(t.flag()).str(t.str()).u32(t.u32());
                if (t.done())
                    return std::move(w.out);
            }
            return std::nullopt;
        }

        // Type properties with the named (symbol, value) pairs removed.
        Bytes withoutProps(const Bytes &raw, const std::set<std::string> &names)
        {
            if (raw.size() <= 1u)
                return raw;
            // Each top-level node's span, then the pairs kept.
            Reader r{raw, 1u};
            const uint16_t count = r.u16();
            r.skip(4u);
            std::vector<std::pair<size_t, size_t>> spans;
            std::vector<std::string> symbols;
            for (uint16_t i = 0; i < count && r.ok; ++i)
            {
                const size_t at = r.o;
                const bool symbol = r.u32() == 5u;
                r.o = at;
                if (symbol)
                {
                    r.skip(4u);
                    symbols.push_back(r.str(65536u));
                }
                else
                {
                    skipNode(r);
                    symbols.emplace_back(); // no property is named ""
                }
                spans.emplace_back(at, r.o);
            }
            if (!r.done())
                return raw;
            Writer w;
            w.u8(1u).u16(0u).raw(raw.data() + 3u, 4u);
            uint16_t kept = 0u;
            for (size_t i = 0; i < spans.size(); i += 2u)
            {
                const bool pair = i + 1u < spans.size();
                if (pair && names.contains(symbols[i]))
                    continue;
                for (size_t j = i; j < std::min(i + 2u, spans.size()); ++j, ++kept)
                    w.raw(raw.data() + spans[j].first, spans[j].second - spans[j].first);
            }
            std::memcpy(w.out.data() + 1u, &kept, 2u);
            return std::move(w.out);
        }

        // Only the Object block changes.
        const std::set<std::string> kObjectAt4 = {
            "AnimFilter",      "Cam",       "CamAnim",     "CharClipGroup", "CharForeTwist", "CharHair",
            "CharIKMidi",      "CharIKRod", "CharPosConstraint", "CharServoBone", "CharUpperTwist", "CharWalk",
            "EnvAnim",         "Environ",   "FaceFxLipSyncServo", "Font",     "Group",         "LightAnim",
            "MatAnim",         "MeshAnim",  "ParticleSysAnim", "ScreenMask",  "Set",           "Text",
            "Trans",           "TransAnim", "Waypoint"};
        // Byte for byte the same on both (SfxSeq 180 of 180).
        const std::set<std::string> kSame = {"SfxSeq", "WaitSeq", "RandomGroupSeq", "SerialGroupSeq"};

        using Rule = std::function<std::optional<Bytes>(const Bytes &)>;
        const std::map<std::string, Rule> kRules = {
            {"Mesh", mesh},
            {"Light", light},
            {"CharClipFilter", clipFilter},
            {"CharDriverMidi", driverMidi},
            {"CamShot", camShot},
            {"LightPreset", lightPreset},
            {"EventTrigger", eventTrigger},
            {"ParticleSys", particleSys},
            {"WorldCrowd", worldCrowd},
            {"CharIKHand", ikHand},
            {"CharDriver", driver},
            {"CharEyes", eyes},
            {"CharLookAt", lookAt},
            {"CharWeightSetter", weightSetter},
            {"CharClipSamples", clipSamples},
            {"CharBone", bone},
            {"Sfx", sfx},
            {"Flare", flare},
        };
    }

    bool noPs2Class(const std::string &className)
    {
        static const std::set<std::string> kNone = {"CubeTex", "PostProc", "FxSendReverb", "TrackWidget",
                                                    "GHTrackDir"};
        return kNone.contains(className);
    }

    bool fromTwin(const std::string &className)
    {
        return className == "SynthSample" || className == "OutfitLoader";
    }

    std::optional<Bytes> toPs2(const std::string &className, const Bytes &body)
    {
        if (const auto rule = kRules.find(className); rule != kRules.end())
            return rule->second(body);
        if (kDirBase.contains(className))
            return dir(className, body, nullptr);
        if (kObjectAt4.contains(className))
            return objectAt4(body);
        if (kSame.contains(className))
            return body;
        return std::nullopt;
    }

    std::optional<Bytes> camShotFocus(const Bytes &body)
    {
        Reader r{body};
        r.rev(27u);
        object(r);
        Writer w;
        put(w.u32(20u), Object{{}, Bytes{0u}});
        r.rev(4u);
        w.u32(4u).raw(r.raw(8u)); // frame, rate
        const uint32_t keys = r.count(1000u);
        w.u32(keys);
        const auto target = [&](const std::string &entity, const std::string &subpart)
        { w.u32(20u).str(entity).str(subpart); };
        const auto skipTarget = [&]
        {
            r.rev(27u);
            r.str();
            r.str();
        };
        for (uint32_t k = 0; k < keys && r.ok; ++k)
        {
            w.raw(r.raw(16u + 48u + 8u)); // duration, blend, ease, fov, transform, screen offset
            r.skip(12u);
            w.f32(0.0f).f32(kFarBlur).f32(kMaxFocalBlur);
            const uint32_t targets = r.count(64u);
            for (uint32_t t = 0; t < targets && r.ok; ++t)
                skipTarget();
            r.rev(27u);
            const std::string entity = r.str(), subpart = r.str();
            w.u32(entity.empty() ? 0u : 1u);
            if (!entity.empty())
                target(entity, subpart);
            skipTarget(); // parent
            target({}, {});
            r.flag();
            r.skip(16u + 4u); // shake, zoom fov
            w.u8(0u).f32(0.0f).f32(0.0f).f32(0.0f).f32(0.0f);
        }
        w.u8(r.flag()).raw(r.raw(12u)); // looping, path ease, near, far
        r.flag();
        w.u8(0u).raw(r.raw(8u)); // depth of field, filter, clamp height
        r.str();
        w.str({}).raw(r.raw(4u)); // path, fade time
        r.str();
        r.skip(4u);
        w.str(kFocusCategory).f32(0.0f); // category, weight
        r.skip(8u * r.count(100000u));
        w.u32(0u).raw(r.raw(4u)); // crowd characters, stamp
        r.strs();
        r.str();
        r.str();
        w.strs({}).str({}).str({}); // hide list, crowd, glow spot
        return finish(r, w);
    }

    std::optional<Mat> matToPs2(const Bytes &body)
    {
        Reader r{body};
        r.rev(28u);
        const Object o = object(r);
        Writer w;
        put(w.u32(27u), o);
        // Blend, colour, prelit, use environ, z mode, alpha cut, alpha
        // write, tex gen, tex wrap, tex transform; diffuse, next pass.
        Mat mat;
        w.raw(r.raw(84u)).str(mat.diffuse = r.str()).str(r.str());
        // Intensify, cull; emissive multiplier, specular colour and power.
        w.u8(r.flag()).u8(r.flag());
        w.f32(mat.emissive = r.f32());
        for (float &value : mat.specular)
            w.f32(value = r.f32());
        for (int i = 0; i < 5; ++i)
        {
            mat.maps.push_back(r.str());
            w.str("");
        }
        w.u8(r.flag()); // per pixel lit
        w.u8(mat.normalize = r.flag());
        r.u32(); // stencil mode
        if (!r.done())
            return std::nullopt;
        mat.body = std::move(w.out);
        return mat;
    }

    // Object, width, height, bits per pixel, source picture, mip bias, type,
    // a byte that is 1 when the bitmap has mips, then the bitmap.
    std::optional<Tex> readTex(const Bytes &body)
    {
        Reader r{body};
        r.rev(10u);
        object(r);
        r.skip(12u);
        r.str();
        r.skip(4u);
        const uint32_t type = r.u32();
        r.u8();
        if (!r.ok)
            return std::nullopt;
        Tex tex;
        if (const auto header = bitmap::read(body.data() + r.o, r.left()))
        {
            tex.picture = type == 1u && header->encoding >= bitmap::kDxt1;
            tex.width = header->width;
            tex.height = header->height;
        }
        return tex;
    }

    std::optional<Bytes> texToPs2(const Bytes &body, const Bytes &bitmap)
    {
        if (bitmap.empty())
            return objectAt4(body);
        const auto header = bitmap::read(bitmap.data(), bitmap.size());
        Reader r{body};
        r.rev(10u);
        const Object o = object(r);
        r.skip(12u);
        const std::string source = r.str();
        const Bytes bias = r.raw(4u);
        const uint32_t type = r.u32();
        if (!r.ok || !header)
            return std::nullopt;
        Writer w;
        put(w.u32(10u), o).u32(header->width).u32(header->height).u32(header->bpp);
        w.str(source).raw(bias).u32(type).u8(0u).raw(bitmap);
        return std::move(w.out);
    }

    std::optional<std::string> spotlightCan(const Bytes &body)
    {
        Reader r{body};
        r.rev(25u);
        object(r);
        draw(r);
        trans(r);
        r.skip(8u);
        r.flag(); // the beam
        r.skip(24u);
        r.str();
        r.skip(12u + 4u);
        r.str();
        r.skip(4u);
        r.strs();
        std::string can = r.str();
        return r.ok ? std::optional(can) : std::nullopt;
    }

    // Spotlight 25 -> 20 (the retail Spotlight::Load 0x273840, BeamDef::Load
    // 0x274270). 25 holds one beam where 20 holds a list (none for one of no
    // length), and a beam loses its brighten, cross section, expand and
    // cutouts.
    std::optional<Bytes> spotlightToPs2(const Bytes &body, const std::string &canGroup)
    {
        Reader r{body};
        r.rev(25u);
        const Object o = object(r);
        Writer w;
        put(w.u32(20u), o).raw(draw(r)).raw(trans(r)).raw(r.raw(8u)); // scale, height
        const bool cone = r.flag();
        const float length = r.f32();
        Writer beam;
        beam.u8(cone).f32(length).raw(r.raw(20u)).str(r.str()).raw(r.raw(12u)); // radii, borders, mat, offsets
        r.skip(4u);
        r.str();
        r.skip(4u);
        r.strs();
        w.u32(length != 0.0f ? 1u : 0u);
        if (length != 0.0f)
            w.raw(beam.out);
        r.str(); // the light can
        w.str(canGroup).str(r.str()).raw(r.raw(4u + 16u + 4u)); // target, can offset, colour, intensity
        w.str(r.str()).raw(r.raw(4u)).str(r.str());             // disc mat, damping, subpart
        w.str(r.str()).raw(r.raw(16u)).raw(r.raw(8u));          // flare mat, sizes, range, steps, offset
        w.u8(r.flag()).u8(r.flag()).raw(r.raw(8u)).str(r.str()); // flare flags, lens size, offset, mat
        w.strs(r.strs()).u8(r.flag()).u8(r.flag());
        return finish(r, w);
    }

    Bytes canGroup(const std::string &mesh)
    {
        static const float kIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        Writer w;
        w.u32(12u).u32(0u).str("").u8(0u);
        w.u32(4u).f32(0.0f).u32(0u);
        w.u32(9u).raw(kIdentity, 48u).raw(kIdentity, 48u).u32(0u).str("").u8(0u).str("");
        w.u32(3u).u8(1u).raw(Bytes(20u, 0u));
        w.strs({mesh}).str("").str("").u32(0u);
        return std::move(w.out);
    }

    std::optional<Bytes> rootToPs2(const std::string &className, const Bytes &root, const std::string &hud)
    {
        if (className == "CharClipSet")
            return clipSet(root);
        if (kDirBase.contains(className))
            return dir(className, root, hud.empty() ? nullptr : &hud);
        return std::nullopt;
    }

    bool inlinedProxy(const std::string &className, const Bytes &body)
    {
        const auto base = kDirBase.find(className);
        if (base == kDirBase.end())
            return false;
        Reader r{body};
        if (className == "WorldDir")
        {
            r.rev(12u);
            r.skip(8u);
            r.str();
        }
        if (className == "WorldDir" || className == "PanelDir")
            r.rev(2u);
        if (className == "BandCharacter")
            r.rev(1u);
        if (base->second == "Character")
            r.rev(10u);
        if (base->second != "ObjectDir")
            r.rev(9u);
        r.rev(17u);
        r.skip(52u * r.count(64u));
        r.u32();
        r.str();
        r.strs();
        return r.u32() != 0u && r.ok;
    }

    std::optional<Bytes> worldFxRoot(const Bytes &root, const Bytes &ps2Root)
    {
        Reader theirs{ps2Root};
        theirs.rev(1u);
        theirs.rev(8u);
        theirs.rev(16u);
        theirs.rev(0u); // the Object's
        const std::string type = theirs.str();
        const auto dirRoot = dir("RndDir", root, nullptr);
        if (!theirs.ok || !dirRoot)
            return std::nullopt;
        Reader ours{*dirRoot};
        ours.skip(8u);
        ours.rev(0u);
        ours.str();
        if (!ours.ok)
            return std::nullopt;
        Writer w;
        w.u32(1u).u32(8u).u32(16u).u32(0u).str(type);
        return std::move(w.raw(dirRoot->data() + ours.o, dirRoot->size() - ours.o).out);
    }

    std::optional<std::string> worldHud(const Bytes &root)
    {
        Reader r{root};
        r.rev(11u);
        r.skip(8u);
        std::string hud = r.str();
        return r.ok ? std::optional(hud) : std::nullopt;
    }
}
