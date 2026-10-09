// GH1's venues in GH2's.
//
// GH2 keeps a venue as a chain of dirs, each the next one's parent:
// world/<v>/gen/<v>.milo (the camera shots), og/gen/<v>_lighting.milo,
// gen/<v>_chars.milo (the band, its waypoints and the crowd) and
// og/gen/<v>_geom.milo (the room). GH1 keeps one as flat scenes:
// venues/<v>/gen/<v>.rnd (the room), lighting.rnd, campaths.rnd, crowd.rnd.
//
// The geom dir here is GH2's own object, cameras, environs and lights, which
// the dirs above name, around GH1's room (gh1/venue_scene.h), with GH1's
// lighting scene beside it. The chars dir is GH2's with its waypoints on
// GH1's spots and GH1's crowd (gh1/crowd.h), and the lighting dir GH2's with
// its spotlights and fixtures unshown. The camera shots are GH1's
// (gh1/cameras.cpp) and the script that drives the anims and lights GH1's
// (gh1/scripts.cpp). Each is in GH1's layer at the stand-in's paths.

#include "gh1/venues.h"

#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/cameras.h"
#include "gh1/crowd.h"
#include "gh1/scene.h"
#include "gh1/scripts.h"
#include "gh1/songs.h"
#include "gh1/sound.h"
#include "gh1/venue_scene.h"
#include "milo/milo.h"

#include <algorithm>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using milo::u32;

        // Hides a GH2 drawable by clearing its Draw 3's showing flag. The
        // Draw follows the Trans 9 in a Mesh 28 or Group 12 (a Group's Anim 4
        // comes before both) and precedes it in a Spotlight 20
        // (Spotlight::Load, 0x273840).
        void hide(const std::string &cls, Bytes &b)
        {
            auto o = headerEnd(b, 4u);
            if (!o)
                return;
            if (cls == "Group" && u32(b, *o) == 4u)
                *o += 12u;
            if (cls != "Spotlight" && u32(b, *o) == 9u)
                *o = transEnd(b, *o);
            if (u32(b, *o) == 3u && *o + 4u < b.size())
                b[*o + 4u] = 0u;
        }

        // GH2's geom dir rebuilt around GH1's room. It keeps GH2's own object
        // and the cameras, environs and lights the dirs above name, plus the
        // materials those dirs' meshes borrow from it (big_chars'
        // crowd_plane.mesh uses ray_blocker.mat) and their textures. GH1's
        // room brings its own environs and lights, and its lighting scene
        // sits beside it. `unreached` collects the meshes GH1 never draws,
        // which must stay hidden even when a script shows them: fest's
        // script shows solo_beam01.mesh, and GH2's dir would draw it.
        std::optional<milo::Dir> geom(const milo::Dir &gh2, const std::vector<const milo::Dir *> &above,
                                      const milo::Dir &room, const milo::Dir &lighting,
                                      const std::set<std::string> &scripted, Drivers &drivers,
                                      std::set<std::string> &unreached)
        {
            const auto root = withoutObjects(gh2.root);
            if (!root)
                return std::nullopt;
            milo::Dir out = gh2;
            out.root = *root;
            out.entries.clear();
            out.bodies.clear();
            static const std::set<std::string> kKept = {"Cam", "Environ", "Light"};
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (kKept.count(gh2.entries[i].first))
                {
                    out.entries.push_back(gh2.entries[i]);
                    out.bodies.push_back(gh2.bodies[i]);
                }
            milo::Dir mats;
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (gh2.entries[i].first == "Mat" &&
                    std::any_of(above.begin(), above.end(), [&](const milo::Dir *d) { return holds(*d, gh2.entries[i].second); }))
                {
                    mats.entries.push_back(gh2.entries[i]);
                    mats.bodies.push_back(gh2.bodies[i]);
                }
            for (size_t i = 0; i < gh2.entries.size(); ++i)
                if (gh2.entries[i].first == "Tex" && holds(mats, gh2.entries[i].second))
                    milo::add(out, "Tex", gh2.entries[i].second, gh2.bodies[i]);
            for (size_t i = 0; i < mats.entries.size(); ++i)
                milo::add(out, "Mat", mats.entries[i].second, std::move(mats.bodies[i]));
            // GH1's camera deletes the room's target_parent.mesh for its own
            // (VenueCam's constructor, GH1 0x16f18c).
            std::set<std::string> taken = {"target_parent.mesh"};
            for (const auto &e : out.entries)
                taken.insert(e.second);
            static const std::set<std::string> kClasses = {
                "Tex", "Mat", "Mesh", "View", "Light", "Environ", "TransAnim", "MatAnim",
                "LightAnim", "EnvAnim", "MeshAnim", "Flare", "ParticleSys", "ParticleSysAnim",
            };
            for (const auto &[scene, top] : {std::pair{&room, kRoomView}, std::pair{&lighting, kLightingView}})
                for (Object &o : objects(*scene, kClasses, top, taken, {}, scripted, drivers))
                    if (taken.insert(o.name).second)
                    {
                        if (o.unreached)
                            unreached.insert(o.name);
                        milo::add(out, o.cls, o.name, std::move(o.body));
                    }
            return out;
        }

        // GH2's lighting dir with its room's spotlights and fixtures hidden.
        milo::Dir lights(const milo::Dir &gh2)
        {
            milo::Dir out = gh2;
            for (size_t i = 0; i < out.entries.size(); ++i)
                if (const std::string &c = out.entries[i].first; c == "Mesh" || c == "Group" || c == "Spotlight")
                    hide(c, out.bodies[i]);
            return out;
        }

        // Where GH1's band stands and its guitarist walks, as the world
        // transforms of stage_spot_NN.mesh (charsys.dta's band_spots puts
        // the singer and keyboard on 01, the bass on 02 and the drummer on
        // 03) and of walk_spot_NN.mesh, where the guitarist starts and walks.
        struct Spots
        {
            std::vector<Bytes> stage, walk;
        };

        Spots spots(const std::vector<const milo::Dir *> &scenes)
        {
            const auto all = [&](const char *prefix)
            {
                std::vector<Bytes> out;
                for (int i = 1;; ++i)
                {
                    const Bytes *mesh = object(scenes, "Mesh", numbered(prefix, i, ".mesh"));
                    if (!mesh || mesh->size() < 104u)
                        return out;
                    out.emplace_back(mesh->begin() + 56, mesh->begin() + 104);
                }
            };
            return {all("stage_spot_"), all("walk_spot_")};
        }

        // Where a Waypoint 3's Trans 9 starts (Waypoint::Load, 0x192068):
        // past Hmx::Object's header and a Draw 3. Its flags follow it.
        std::optional<size_t> waypointTrans(const Bytes &b)
        {
            const auto o = headerEnd(b, 4u);
            if (!o || u32(b, 0u) != 3u || u32(b, *o) != 3u || u32(b, *o + 25u) != 9u)
                return std::nullopt;
            return *o + 25u;
        }

        // A copy of Waypoint 3 `from` moved onto that spot, with those flags
        // and links (Waypoint::Load, 0x192068).
        Bytes waypoint(const Bytes &from, size_t trans, const Bytes &spot, uint32_t flags,
                       const std::vector<std::string> &links)
        {
            size_t o = transEnd(from, trans);
            Bytes out(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(o));
            for (const size_t t : {trans + 4u, trans + 52u})
                std::copy(spot.begin(), spot.end(), out.begin() + static_cast<std::ptrdiff_t>(t));
            putU32(out, flags);
            putNames(out, links);
            const uint32_t had = u32(from, o + 4u);
            o += 8u;
            for (uint32_t i = 0; i < had; ++i)
                str(from, o);
            put(out, from, o, from.size());
            return out;
        }

        // GH2's chars dir with its waypoints replaced by GH1's (flags in
        // macros.dta). The band starts on GH1's stage spots and the
        // guitarist on walk spot 01. GH1 has no second guitarist, so with two
        // they start on 02 and 01. Each walk spot gets a waypoint. The last
        // is where a walk turns, never where one ends (StartWalk, GH1
        // 0x284818), and the others link to it. Those others are walked to
        // only if the venue `walks`, `solo` being the one before a solo, and
        // their names go in `onWalk`.
        std::optional<milo::Dir> chars(const milo::Dir &gh2, const Spots &at, bool walks, int solo,
                                       std::vector<std::string> &onWalk)
        {
            if (at.stage.size() < 3u || at.walk.empty())
                return std::nullopt;
            enum : uint32_t
            {
                kStartGuitarist0 = 1u,
                kStartGuitarist1Mp = 2u,
                kStartSinger = 4u,
                kStartKeyboardist = 8u,
                kStartBassist = 16u,
                kStartDrummer = 32u,
                kWalkSpot = 64u,
                kSoloWalkSpot = 128u,
                kStartGuitarist0Mp = 512u,
                kStarts = kStartGuitarist0 | kStartGuitarist1Mp | kStartSinger | kStartKeyboardist | kStartBassist |
                          kStartDrummer | kStartGuitarist0Mp,
            };
            milo::Dir out = gh2;
            out.entries.clear();
            out.bodies.clear();
            std::optional<std::pair<Bytes, size_t>> start; // the guitarist's, and its Trans
            for (size_t i = 0; i < gh2.entries.size(); ++i)
            {
                if (gh2.entries[i].first != "Waypoint")
                {
                    out.entries.push_back(gh2.entries[i]);
                    out.bodies.push_back(gh2.bodies[i]);
                    continue;
                }
                const Bytes &b = gh2.bodies[i];
                const auto trans = waypointTrans(b);
                if (!trans)
                    return std::nullopt;
                const uint32_t flags = u32(b, transEnd(b, *trans));
                const Bytes *spot = nullptr;
                if (flags & kStartGuitarist0)
                    spot = &at.walk[0];
                else if (flags & (kStartSinger | kStartKeyboardist))
                    spot = &at.stage[0];
                else if (flags & kStartBassist)
                    spot = &at.stage[1];
                else if (flags & kStartDrummer)
                    spot = &at.stage[2];
                else if (flags & kStartGuitarist0Mp)
                    spot = &at.walk[at.walk.size() > 1u ? 1u : 0u];
                else if (flags & kStartGuitarist1Mp)
                    spot = &at.walk[0];
                if (!spot)
                    continue;
                out.entries.push_back(gh2.entries[i]);
                out.bodies.push_back(waypoint(b, *trans, *spot, flags & kStarts, {}));
                if (flags & kStartGuitarist0)
                    start = {out.bodies.back(), *trans};
            }
            if (!start)
                return std::nullopt;
            const size_t spots = at.walk.size() > 1u ? at.walk.size() - 1u : 1u;
            onWalk.clear();
            for (size_t i = 0; i < at.walk.size(); ++i)
                onWalk.push_back(numbered("walk_spot_", static_cast<int>(i) + 1, ".way"));
            for (size_t i = 0; i < at.walk.size(); ++i)
            {
                const bool turn = i >= spots;
                const uint32_t flags =
                    turn || !walks ? 0u : kWalkSpot | (static_cast<int>(i) == solo ? kSoloWalkSpot : 0u);
                std::vector<std::string> links;
                if (turn)
                    links.assign(onWalk.begin(), onWalk.begin() + static_cast<std::ptrdiff_t>(spots));
                else if (spots < at.walk.size())
                    links.push_back(onWalk[spots]);
                milo::add(out, "Waypoint", onWalk[i], waypoint(start->first, start->second, at.walk[i], flags, links));
            }
            onWalk.resize(spots);
            return out;
        }
    }

    void addVenues(size_t layer, size_t disc)
    {
        // GH1's venues, each with the View that is its drum kit: part of the
        // room, where GH2's is its drummer's.
        static const std::pair<const char *, const char *> kVenues[] = {
            {"basement", "drum_kit.view"}, {"small_club", "drumkit.view"}, {"big_club", "drum_kit.view"},
            {"theatre", "drum_kit.view"},  {"fest", "drum_kit.view"},      {"arena", "drum kit 00.view"},
        };
        dtb::Macros none;
        const dtb::Files files = [disc](const std::string &path) { return ark::readFile(disc, path); };
        // How high a flat crowd member's card is (crowd_flat_height in
        // system/run/config/arena.dta, merged into config/gh.dta's arena).
        float flatHeight = 100.8f;
        if (const auto config = dtb::read("config/gh.dta", none, files))
            if (const dtb::Node *arena = dtb::find(*config, "arena"))
                if (const dtb::Node *found = dtb::find(*arena, "crowd_flat_height"); found && found->nodes.size() > 1u)
                    flatHeight = dtb::number(found->nodes[1]).value_or(flatHeight);
        // Whether the guitarist walks in each venue, and the walk spot a
        // solo is played on (allow_walks and solo_walk_point, by its number
        // less one, in arena/venues.dta).
        const auto settings = dtb::read("arena/venues.dta", none, files);
        const auto setting = [&](const char *of, const char *key, float fallback)
        {
            const dtb::Node *theirs = settings ? dtb::find(*settings, of) : nullptr;
            const dtb::Node *found = theirs ? dtb::find(*theirs, key) : nullptr;
            return found && found->nodes.size() > 1u ? dtb::number(found->nodes[1]).value_or(fallback) : fallback;
        };
        // The pool a follow spot throws on the floor, from the scene every
        // venue shares, in the venue's own floorspot_glow.mat
        // (VenueSpotLight's constructor, GH1 0x179190).
        const auto fx = loadScene(disc, "../../system/run/arena/gen/fx.rnd_ps2");
        for (const auto &[name, kit] : kVenues)
        {
            const std::string ours = venue(name);
            const std::string world = "world/" + ours + "/", theirs = std::string("venues/") + name + "/gen/";
            const std::string geomPath = world + "og/gen/" + ours + "_geom.milo_ps2";
            const std::string lightsPath = world + "og/gen/" + ours + "_lighting.milo_ps2";
            const std::string charsPath = world + "gen/" + ours + "_chars.milo_ps2";
            const auto gh2Geom = loadScene(0u, geomPath);
            const auto gh2Lights = loadScene(0u, lightsPath);
            const auto gh2Chars = loadScene(0u, charsPath);
            const auto room = loadScene(disc, theirs + name + ".rnd_ps2");
            const auto lighting = loadScene(disc, theirs + "lighting.rnd_ps2");
            if (!gh2Geom || !gh2Lights || !gh2Chars || !room || !lighting)
            {
                std::cerr << "[gh1] cannot read " << name << "'s venue" << std::endl;
                continue;
            }
            Drivers drivers;
            std::set<std::string> unreached;
            auto madeGeom =
                geom(*gh2Geom, {&*gh2Chars, &*gh2Lights}, *room, *lighting, scripted(disc, name), drivers, unreached);
            const Spots at = spots({&*lighting, &*room});
            Stage stage;
            auto madeChars = chars(*gh2Chars, at, setting(name, "allow_walks", 0.0f) != 0.0f,
                                   static_cast<int>(setting(name, "solo_walk_point", -1.0f)), stage.walks);
            if (!madeGeom || !madeChars)
            {
                std::cerr << "[gh1] cannot build " << name << "'s venue" << std::endl;
                continue;
            }
            if (const Bytes *pool = object({fx ? &*fx : nullptr}, "Mesh", "floorspot_char.mesh");
                pool && milo::find(*madeGeom, "floorspot_glow.mat") && milo::find(*madeGeom, "spotlight01.lit"))
                milo::add(*madeGeom, "Mesh", "floorspot_char.mesh", *pool);
            const auto crowdScene = loadScene(disc, theirs + "crowd.rnd_ps2");
            const std::vector<const milo::Dir *> crowdScenes = {&*lighting, &*room, crowdScene ? &*crowdScene : nullptr};
            const std::vector<Bytes> places = crowdPlaces(crowdScenes);
            stage.crowdStamp = crowd(*madeChars, places, flatHeight);
            // The members GH1 draws whole: arena/crowd.dta's instances, ten,
            // two more outside the festival, and five more again in the
            // basement and the small club.
            const std::string venueName = name;
            const size_t whole = venueName == "fest" ? 10u : venueName == "basement" || venueName == "small_club" ? 17u : 12u;
            stage.regions = crowdRegions(crowdScenes, places, flatHeight, whole);
            ark::addFile(layer, geomPath, milo::write(*madeGeom));
            ark::addFile(layer, lightsPath, milo::write(lights(*gh2Lights)));
            ark::addFile(layer, charsPath, milo::write(*madeChars));
            stage.spot = at.stage[0];
            addCameras(layer, disc, name, ours, stage);
            std::set<std::string> present;
            for (const auto &e : madeGeom->entries)
                if (!unreached.count(e.second))
                    present.insert(e.second);
            addScripts(layer, disc, name, ours, drivers, present, kit, stage.walks);
            // The stand-in's sound bank (its type's (sound (bank ...)),
            // GamePanel::RetainWorldBank, 0x108f00), silenced: GH1's
            // arena_game.dta names crowd cheers, claps and a lost big note,
            // but no GH1 bank holds them.
            const std::string bankPath = world + "gen/" + ours + "_bank.milo_ps2";
            if (const auto standIn = ark::readFile(0u, bankPath))
                if (auto quiet = silenced(*standIn))
                    ark::addFile(layer, bankPath, std::move(*quiet));
            // GH1's crowd streams, which the type names (gh1/scripts.cpp).
            // What is not made here is the stand-in's: its encore streams.
            const std::string streams = std::string("venues/") + name + "/streams/";
            for (int version = 1; version < 10; ++version)
                for (const char *level : {"0intro", "1danger", "2poor", "3norm", "4good"})
                {
                    const std::string file = "crowd_v" + std::to_string(version) + "_" + level + ".vgs";
                    ark::lend(layer, world + "streams/" + file, disc, streams + file);
                }
        }
    }
}
