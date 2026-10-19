// "Classic" under each game's logo: tools/procedural/classic_logo.py's
// bitmap on a copy of the help bar's help_box_right.mesh, the one quad there
// that maps a whole texture (mirrored, which the bitmap is drawn to undo),
// 16 by 16 from the middle of its left edge. help_box_mid's Tex and Mat give
// the bitmap's format and the Mat's own fields.

#include "ui/classic_scene.h"

#include "classic_logo.h"
#include "content/games.h"
#include "disc/ark.h"
#include "gh1/scene.h"
#include "milo/objects.h"

#include <cstring>
#include <iostream>
#include <optional>

namespace gh2
{
    namespace
    {
        constexpr const char *kMesh = "ghc_classic.mesh", *kMat = "ghc_classic.mat", *kTex = "ghc_classic.tex";

        // Where, in the space of the screen's top View (main.view,
        // splash.view): the origin at the screen's middle, z up, a unit 2.26
        // pixels at 1080p. x and z are the quad's middle.
        struct Place
        {
            float x, z, width, height;
        };
        struct Places
        {
            const char *game;
            Place main, splash;
        };
        constexpr Places kPlaces[] = {
            {"gh1", {-1.6f, 10.1f, 240.0f, 45.0f}, {0.7f, -85.1f, 354.5f, 66.5f}},
            {"gh2", {-19.8f, 56.6f, 244.3f, 45.8f}, {-30.2f, -5.0f, 239.1f, 44.8f}},
            {"gh80s", {2.2f, 113.7f, 112.7f, 21.1f}, {8.5f, 23.2f, 225.5f, 42.3f}},
        };

        struct HelpBar
        {
            milo::Bytes mesh, tex, mat;
        };

        // GH2's help bar's, as its disc has them.
        const std::optional<HelpBar> &helpBar()
        {
            static const std::optional<HelpBar> parts = []() -> std::optional<HelpBar>
            {
                const auto file = ark::readFile(0u, "ui/gen/helpbar.milo_ps2");
                const auto dir = file ? milo::read(*file) : std::nullopt;
                const auto mesh = dir ? milo::find(*dir, "help_box_right.mesh") : std::nullopt;
                const auto tex = dir ? milo::find(*dir, "help_box_mid.tex") : std::nullopt;
                const auto mat = dir ? milo::find(*dir, "help_box_mid.mat") : std::nullopt;
                if (!mesh || !tex || !mat)
                    return std::nullopt;
                return HelpBar{dir->bodies[*mesh], dir->bodies[*tex], dir->bodies[*mat]};
            }();
            return parts;
        }

        // The quad from its left edge's middle, `place` in its parent's
        // space: the local and the world transform (Trans, past the Mesh's
        // and Object's revisions and its own), as the View sits at the
        // origin. Then it is drawn, which the help bar's is not without a
        // box up.
        bool placeQuad(milo::Bytes &mesh, const Place &place)
        {
            const float xfm[12] = {place.width / 16.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                                   place.height / 16.0f, place.x - place.width / 2.0f, 0.0f, place.z};
            constexpr size_t kLocal = 17u, kWorld = kLocal + sizeof xfm;
            // Its constraint, target, preserve_scale and parent, then Draw's
            // revision and showing.
            size_t o = kWorld + sizeof xfm + 4u;
            milo::str(mesh, o);
            ++o;
            milo::str(mesh, o);
            o += 4u;
            if (o >= mesh.size())
                return false;
            std::memcpy(mesh.data() + kLocal, xfm, sizeof xfm);
            std::memcpy(mesh.data() + kWorld, xfm, sizeof xfm);
            mesh[o] = 1u;
            return true;
        }
    }

    bool addClassicLogo(milo::Dir &scene, const std::string &game, const std::string &screen)
    {
        const bool splash = screen == "splash";
        if ((!splash && screen != "main") || milo::find(scene, kMesh))
            return false;
        const Places *places = nullptr;
        for (const Places &p : kPlaces)
            if (game == p.game)
                places = &p;
        const ClassicLogo *logo = nullptr;
        for (const ClassicLogo &l : kClassicLogos)
            if (game == l.game)
                logo = &l;
        const auto &parts = helpBar();
        if (!places || !logo || !parts)
            return false;

        milo::Bytes tex = gh1::texWith(parts->tex, milo::Bytes(logo->bitmap, logo->bitmap + logo->size), true);
        milo::Bytes mat = parts->mat, mesh = parts->mesh;
        const std::string view = splash ? "splash.view" : "main.view";
        if (tex.empty() || !milo::replaceStr(mat, "help_box_mid.tex", kTex) || !milo::setMatDrawn(mat, scene.version, 3u, 3u) ||
            !milo::replaceStr(mesh, "help_box_corner.mat", kMat) || !milo::replaceStr(mesh, "help_box_right.mesh", kMesh) ||
            !milo::replaceStr(mesh, "helpbar.view", view) || !placeQuad(mesh, splash ? places->splash : places->main))
        {
            std::cerr << "[classic] cannot build " << game << "'s " << screen << std::endl;
            return false;
        }

        // Drawn as the menu's first button, which the others draw over, or
        // just over the splash's poster.
        const char *beside = splash ? "splash_poster.mesh" : "main_career.btn";
        bool placed = false;
        for (size_t i = 0; i < scene.entries.size() && !placed; ++i)
            if (scene.entries[i].first == "Group" || scene.entries[i].first == "View")
                placed = splash ? milo::addAfter(scene.bodies[i], beside, kMesh) : milo::addBefore(scene.bodies[i], beside, kMesh);
        if (!placed)
        {
            std::cerr << "[classic] no " << beside << " in " << game << "'s " << screen << std::endl;
            return false;
        }
        milo::add(scene, "Tex", kTex, std::move(tex));
        milo::add(scene, "Mat", kMat, std::move(mat));
        milo::add(scene, "Mesh", kMesh, std::move(mesh));
        return true;
    }

    void installClassicLogo()
    {
        for (const char *game : {"gh2", "gh80s"})
        {
            const auto disc = std::string(game) == "gh2" ? std::optional<size_t>(0u) : games::disc(game);
            if (!disc)
                continue;
            for (const char *screen : {"main", "splash"})
                ark::addFix(*disc, std::string("ui/gen/") + screen + ".milo_ps2",
                            [game = std::string(game), screen = std::string(screen)](std::vector<uint8_t> &file)
                            {
                                auto scene = milo::read(file);
                                if (scene && addClassicLogo(*scene, game, screen))
                                    file = milo::write(*scene);
                            });
        }
    }
}
