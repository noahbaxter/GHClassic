// The campaign picker's poster and cases (content/campaign_case.h), each
// Mesh made from the speed poster's (hub/poster_meshes.h).

#include "content/campaign_case.h"

#include "campaign_pack.h"
#include "content/games.h"
#include "disc/ark.h"
#include "hub/poster_meshes.h"
#include "milo/milo.h"
#include "render/texture_store.h"

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
        using poster_meshes::Mesh;
        using milo::body;
        using poster_meshes::meshBody;

        constexpr const char *kScene = "ui/gen/sel_speed.milo_ps2";
        constexpr const char *kPoster = "speed_poster";
        // The plastic's light, toward it in the case's own axes, of length
        // 1: mostly from the opening side, so that edge's flat face is
        // bright and its grip and seam, curved away, fall dark.
        constexpr std::array kLight = {0.8f, -0.48f, 0.36f};

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

        // A game's front cover alone, flat, the case's size: the wrap's
        // right part from where the front starts (x 1700 of a 3225 scan,
        // the spine before it).
        Mesh cover()
        {
            return poster_meshes::quad(1.195f, 1.69f, 1700.0f / 3225.0f, 1.0f);
        }

        // A flat ring, 1 across its outside, facing front.
        Mesh ring()
        {
            constexpr int kSides = 48;
            constexpr float kInside = 0.84f;
            Mesh ring;
            for (int i = 0; i < kSides; ++i)
            {
                const float a = 2.0f * 3.14159265f * static_cast<float>(i) / kSides;
                for (const float r : {1.0f, kInside})
                    ring.verts.insert(ring.verts.end(), {r * std::cos(a), 0, r * std::sin(a), 0, -1, 0, 0, 0});
                const auto at = [](int side, int inner) { return static_cast<uint16_t>((side % kSides) * 2 + inner); };
                ring.faces.insert(ring.faces.end(), {at(i, 0), at(i, 1), at(i + 1, 0), at(i + 1, 0), at(i, 1), at(i + 1, 1)});
            }
            return ring;
        }

        // A Tex like `from` whose bitmap names that bundled picture.
        Bytes texOf(const Bytes &from, const std::string &picture)
        {
            return poster_meshes::texOf(from, {textures::kBundled, picture, ""});
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
                const std::string posterMat = std::string(kPoster) + ".mat";
                const auto sleeveMesh = meshBody(mesh, posterMat, pack.sleeve, "ghc_case_" + game + "_sleeve.mesh", sleeveMat);
                // The plastic's colour is in its verts too, the 360's green,
                // a PS2's black: its Mat is prelit (campaigns.dta), which
                // draws the vertex colour alone.
                const std::array<float, 3> plastic =
                    game == "gh2x" ? std::array{0.3f, 0.75f, 0.2f} : std::array{0.32f, 0.32f, 0.35f};
                const auto shellMesh =
                    meshBody(mesh, posterMat, pack.shell, "ghc_case_" + game + "_shell.mesh", shellMat, &kLight, plastic);
                const auto coverMesh = meshBody(mesh, posterMat, cover(), "ghc_cover_" + game + ".mesh", sleeveMat);
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
            // The note engine's mark (campaigns.dta): a ring in the title's
            // grey blue, its colour in its verts as the plastic's is, with
            // the engine's numeral over it in the title's own font.
            Bytes ringMat = mat;
            milo::replaceStr(ringMat, std::string(kPoster) + ".tex", "");
            const auto ringMesh = meshBody(mesh, std::string(kPoster) + ".mat", ring(), "ghc_engine.mesh", "ghc_engine.mat",
                                           nullptr, {0.56f, 0.66f, 0.76f});
            if (!ringMesh)
                return false;
            milo::add(dir, "Mat", "ghc_engine.mat", std::move(ringMat));
            milo::add(dir, "Mesh", "ghc_engine.mesh", *ringMesh);
            if (Bytes *group = body(dir, "Group", "background.view"))
                milo::addAfter(*group, std::string(kPoster) + ".mesh", "ghc_engine.mesh");
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
