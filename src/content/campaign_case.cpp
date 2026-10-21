// The campaign picker's poster and cases (content/campaign_case.h).
//
// Each Mesh is GH2's Mesh 28 as its loader reads it (RndMesh::Load,
// 0x3d5420): the speed poster's own up to its Mat's name, which keeps its
// Object, Trans and Draw, then the case's Mat, its own name as the
// geometry's owner, the poster's mutable and volume words and no BSP, the
// verts (position, normal, colour, uv), the faces, their groups of up to 255
// and no bones, then their PS2 strips (milo::strips).

#include "content/campaign_case.h"

#include "campaign_pack.h"
#include "content/games.h"
#include "disc/ark.h"
#include "gh1/scene.h"
#include "milo/milo.h"
#include "render/texture_store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::campaign_case
{
    namespace
    {
        using milo::Bytes;

        constexpr const char *kScene = "ui/gen/sel_speed.milo_ps2";
        constexpr const char *kPoster = "speed_poster";
        // The guest's stand-in for a picture the host draws: its size only
        // counts in the guest's memory.
        constexpr uint32_t kStandIn = 64u;
        // The plastic's light, toward it in the case's own axes, of length
        // 1: mostly from the opening side, so that edge's flat face is
        // bright and its grip and seam, curved away, fall dark.
        constexpr float kLight[3] = {0.8f, -0.48f, 0.36f};

        struct Mesh
        {
            std::vector<float> verts; // 8 a vert: position, normal, uv
            std::vector<uint16_t> faces;
        };

        struct Pack
        {
            Mesh sleeve, shell;
            std::map<std::string, std::pair<const uint8_t *, size_t>> pictures;
        };

        std::optional<Pack> readPack()
        {
            const uint8_t *b = kCampaignPack;
            const size_t size = sizeof kCampaignPack;
            size_t o = 0u;
            const auto word = [&]() -> std::optional<uint32_t>
            {
                if (o + 4u > size)
                    return std::nullopt;
                uint32_t v = 0u;
                std::memcpy(&v, b + o, 4u);
                o += 4u;
                return v;
            };
            if (size < 8u || std::memcmp(b, "GHCP", 4u) != 0)
                return std::nullopt;
            o = 8u;
            Pack pack;
            const auto meshes = word();
            if (!meshes || *meshes != 2u)
                return std::nullopt;
            for (Mesh *mesh : {&pack.sleeve, &pack.shell})
            {
                const auto verts = word();
                if (!verts || o + *verts * 32u > size)
                    return std::nullopt;
                mesh->verts.resize(*verts * 8u);
                std::memcpy(mesh->verts.data(), b + o, *verts * 32u);
                o += *verts * 32u;
                const auto faces = word();
                if (!faces || o + *faces * 6u > size)
                    return std::nullopt;
                mesh->faces.resize(*faces * 3u);
                std::memcpy(mesh->faces.data(), b + o, *faces * 6u);
                o += *faces * 6u;
            }
            const auto pictures = word();
            for (uint32_t i = 0u; pictures && i < *pictures; ++i)
            {
                const auto length = word();
                if (!length || o + *length > size)
                    return std::nullopt;
                const std::string name(reinterpret_cast<const char *>(b + o), *length);
                o += *length;
                const auto bytes = word();
                if (!bytes || o + *bytes > size)
                    return std::nullopt;
                pack.pictures[name] = {b + o, *bytes};
                o += *bytes;
            }
            return pack;
        }

        void putF32(Bytes &out, float v)
        {
            uint32_t u = 0u;
            std::memcpy(&u, &v, 4u);
            milo::putU32(out, u);
        }

        // Where a body holds `name` as a string, its length word's offset.
        std::optional<size_t> stringAt(const Bytes &body, const std::string &name)
        {
            for (size_t o = 0u; o + 4u + name.size() <= body.size(); ++o)
                if (milo::u32(body, o) == name.size() && std::memcmp(body.data() + o + 4u, name.data(), name.size()) == 0)
                    return o;
            return std::nullopt;
        }

        // A mesh's body made from the poster's (`poster`), lit by kLight or,
        // flat, not at all.
        std::optional<Bytes> meshBody(const Bytes &poster, const Mesh &mesh, const std::string &name,
                                      const std::string &mat, bool lit = true,
                                      const std::array<float, 3> &tint = {1.0f, 1.0f, 1.0f})
        {
            const auto at = stringAt(poster, std::string(kPoster) + ".mat");
            if (!at || *at < 20u)
                return std::nullopt;
            size_t o = *at;
            milo::str(poster, o); // the poster's Mat
            milo::str(poster, o); // its geometry's owner
            const size_t words = o;
            Bytes out(poster.begin(), poster.begin() + static_cast<std::ptrdiff_t>(*at));
            milo::putStr(out, mat);
            milo::putStr(out, name);
            out.insert(out.end(), poster.begin() + static_cast<std::ptrdiff_t>(words),
                       poster.begin() + static_cast<std::ptrdiff_t>(words + 8u));
            out.push_back(0u); // no BSP tree
            const uint32_t verts = static_cast<uint32_t>(mesh.verts.size() / 8u);
            milo::putU32(out, verts);
            for (uint32_t v = 0u; v < verts; ++v)
            {
                const float *f = mesh.verts.data() + v * 8u;
                for (int i = 0; i < 6; ++i)
                    putF32(out, f[i]);
                // No light reaches a menu: a light baked in, from above and
                // to the right of the front, and as much from the back, so
                // the plastic's edges and the grip on the opening side read
                // as the case turns.
                const float toward = std::fabs(kLight[0] * f[3] + kLight[1] * f[4] + kLight[2] * f[5]);
                const float shade = lit ? 0.15f + 0.85f * toward : 1.0f;
                for (int i = 0; i < 3; ++i)
                    putF32(out, shade * tint[i]);
                putF32(out, 1.0f);
                putF32(out, f[6]);
                putF32(out, f[7]);
            }
            const uint32_t faces = static_cast<uint32_t>(mesh.faces.size() / 3u);
            const auto *faceBytes = reinterpret_cast<const uint8_t *>(mesh.faces.data());
            milo::putU32(out, faces);
            out.insert(out.end(), faceBytes, faceBytes + 6u * faces);
            std::vector<uint32_t> groups;
            for (uint32_t left = faces; left > 0u; left -= std::min(left, 255u))
                groups.push_back(std::min(left, 255u));
            milo::putU32(out, static_cast<uint32_t>(groups.size()));
            for (const uint32_t g : groups)
                out.push_back(static_cast<uint8_t>(g));
            milo::putStr(out, ""); // no bones
            const Bytes strips = milo::strips(faceBytes, groups);
            out.insert(out.end(), strips.begin(), strips.end());
            return out;
        }

        // A game's front cover alone, flat, the case's size: the wrap's
        // right part from where the front starts (x 1700 of a 3225 scan,
        // the spine before it).
        Mesh cover()
        {
            constexpr float kFront = 1700.0f / 3225.0f;
            constexpr float kW = 1.195f, kH = 1.69f;
            Mesh quad;
            quad.verts = {-kW, 0, -kH, 0, -1, 0, kFront, 1, kW, 0, -kH, 0, -1, 0, 1, 1,
                          kW,  0, kH,  0, -1, 0, 1,      0, -kW, 0, kH, 0, -1, 0, kFront, 0};
            quad.faces = {0, 1, 2, 0, 2, 3};
            return quad;
        }

        Bytes *body(milo::Dir &dir, const std::string &cls, const std::string &name)
        {
            const auto it = std::find(dir.entries.begin(), dir.entries.end(), std::make_pair(cls, name));
            return it == dir.entries.end() ? nullptr : &dir.bodies[static_cast<size_t>(it - dir.entries.begin())];
        }

        // A Tex like `from` whose bitmap names that picture.
        Bytes texOf(const Bytes &from, const std::string &picture)
        {
            const uint32_t id = textures::add({textures::kBundled, picture, ""});
            return gh1::texWith(from, textures::standIn(id, kStandIn, kStandIn), true);
        }

        bool addCases(milo::Dir &dir, const Pack &pack)
        {
            const Bytes *posterTex = body(dir, "Tex", std::string(kPoster) + ".tex");
            const Bytes *posterMat = body(dir, "Mat", std::string(kPoster) + ".mat");
            const Bytes *posterMesh = body(dir, "Mesh", std::string(kPoster) + ".mesh");
            if (!posterTex || !posterMat || !posterMesh)
                return false;
            const Bytes tex = *posterTex, mat = *posterMat, mesh = *posterMesh;
            // The poster: the Demo's amp in place of the speed picker's, a
            // Tex of its own, which the 360's art (gh2x/textures.cpp),
            // swapped in by the speed poster's name, leaves alone.
            if (pack.pictures.contains("poster"))
            {
                milo::replaceStr(*body(dir, "Mat", std::string(kPoster) + ".mat"), std::string(kPoster) + ".tex",
                                 "ghc_amp.tex");
                milo::add(dir, "Tex", "ghc_amp.tex", texOf(tex, "campaign/poster"));
            }
            // A case for each game the picker lists: GH2, and each other
            // whose disc is mounted.
            for (const auto &[game, picture] : pack.pictures)
            {
                if (game == "poster" || (game != "gh2" && !games::mounted(game)))
                    continue;
                const std::string texName = "ghc_case_" + game + ".tex";
                const std::string sleeveMat = "ghc_case_" + game + ".mat", shellMat = "ghc_shell_" + game + ".mat";
                Bytes sleeve = mat, shell = mat;
                milo::replaceStr(sleeve, std::string(kPoster) + ".tex", texName);
                milo::replaceStr(shell, std::string(kPoster) + ".tex", "");
                // The sleeve is printed card: its own colours, unlit.
                const auto sleeveMesh =
                    meshBody(mesh, pack.sleeve, "ghc_case_" + game + "_sleeve.mesh", sleeveMat, false);
                // The plastic's colour is in its verts too, the 360's green,
                // a PS2's black: its Mat is prelit (campaigns.dta), which
                // draws the vertex colour alone.
                const std::array<float, 3> plastic =
                    game == "gh2x" ? std::array{0.3f, 0.75f, 0.2f} : std::array{0.32f, 0.32f, 0.35f};
                const auto shellMesh =
                    meshBody(mesh, pack.shell, "ghc_case_" + game + "_shell.mesh", shellMat, true, plastic);
                const auto coverMesh = meshBody(mesh, cover(), "ghc_cover_" + game + ".mesh", sleeveMat, false);
                if (!sleeveMesh || !shellMesh || !coverMesh)
                    return false;
                milo::add(dir, "Tex", texName, texOf(tex, "campaign/" + game));
                milo::add(dir, "Mat", sleeveMat, std::move(sleeve));
                milo::add(dir, "Mat", shellMat, std::move(shell));
                milo::add(dir, "Mesh", "ghc_case_" + game + "_sleeve.mesh", *sleeveMesh);
                milo::add(dir, "Mesh", "ghc_case_" + game + "_shell.mesh", *shellMesh);
                milo::add(dir, "Mesh", "ghc_cover_" + game + ".mesh", *coverMesh);
                // Drawn where the poster is (background.view lists it; its
                // own group only places it), after it: the sleeve inside
                // its plastic.
                if (Bytes *group = body(dir, "Group", "background.view"))
                {
                    milo::addAfter(*group, std::string(kPoster) + ".mesh", "ghc_cover_" + game + ".mesh");
                    milo::addAfter(*group, std::string(kPoster) + ".mesh", "ghc_case_" + game + "_shell.mesh");
                    milo::addAfter(*group, std::string(kPoster) + ".mesh", "ghc_case_" + game + "_sleeve.mesh");
                }
            }
            return true;
        }
    }

    void install()
    {
        static const std::optional<Pack> pack = readPack();
        if (!pack)
        {
            std::cerr << "[campaign_case] cannot read the pack" << std::endl;
            return;
        }
        for (const auto &[name, picture] : pack->pictures)
            textures::addBundled("campaign/" + name, picture.first, picture.second);
        // The picker in GH2's skin and the 80s', each its disc's own scene.
        for (const char *game : {"gh2", "gh80s"})
            if (const auto disc = games::disc(game))
                ark::addFix(*disc, kScene, [](std::vector<uint8_t> &file)
                            {
                                auto scene = milo::read(file);
                                if (scene && addCases(*scene, *pack))
                                {
                                    file = milo::write(*scene);
                                    std::cout << "[campaign_case] " << kScene << ": poster and cases" << std::endl;
                                }
                                else
                                    std::cerr << "[campaign_case] cannot add the cases to " << kScene << std::endl;
                            });
    }
}
