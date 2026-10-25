#include "hub/hub_scene.h"

#include "content/games.h"
#include "content/outfits.h"
#include "hub/poster_meshes.h"
#include "hub/venue_posters.h"
#include "milo/milo.h"
#include "render/texture_store.h"

#include <cstring>
#include <string>
#include <vector>

namespace gh2::hub_scene
{
    namespace
    {
        // Each game's venues ({campaigns venues}), GH1's by the GH2 venues
        // standing in for them, and where its venue map's drawing of each
        // is: GH2's and the 80s' a Tex in their map's scene, GH1's a bitmap
        // of its own, named a little apart from the venue.
        // A venue's drawing (`art`, its name there); with none, "unknown"'s,
        // song defaults' poster, a map page of its own, which no stand-in
        // shows otherwise.
        textures::Source artOf(const std::string &game, size_t disc, const char *art)
        {
            if (!art)
                return game == "gh1" ? textures::Source{disc, "ghui/image/gen/map1.bmp_ps2", "", true}
                                     : textures::Source{disc, "ui/gen/sel_venue.milo_ps2", "sv_map1.tex", true};
            if (game == "gh1")
                return {disc, std::string("ghui/image/gen/venue_") + art + ".png_ps2", "", true};
            return {disc, "ui/gen/sel_venue.milo_ps2", std::string("sv_venue_") + art + ".tex", true};
        }

        // The portrait the venues are copied from, the Group drawing it, which
        // draws each venue after it, and the Tex names its Mat holds.
        constexpr const char *kMesh = "char_punk.mesh", *kMat = "char_punk.mat", *kTex = "char_punk.tex";
        constexpr const char *kGroup = "cs_icons.grp";

        // A flat picture of each mounted game's venues, named for its game
        // and venue (ghc_venue_<game>_<venue>.mesh, .mat and .tex), drawn
        // after the portrait.
        bool addVenues(milo::Dir &scene)
        {
            const milo::Bytes *likeTex = milo::body(scene, "Tex", kTex);
            const milo::Bytes *likeMat = milo::body(scene, "Mat", kMat);
            const milo::Bytes *likeMesh = milo::body(scene, "Mesh", kMesh);
            if (!likeTex || !likeMat || !likeMesh)
                return false;
            const milo::Bytes tex = *likeTex, mat = *likeMat, mesh = *likeMesh;
            // Its poster's shape (hub/venue_posters.h), across from u 1 to
            // 0: the hub stands it in the open door's face seen from behind
            // (quickplay_setup.dta ghc_qp_show_venue).
            const poster_meshes::Mesh picture =
                poster_meshes::quad(1.0f, float(venue_posters::kHeight) / float(venue_posters::kWidth), 1.0f, 0.0f);
            for (const char *game : {"gh2", "gh80s", "gh1"})
            {
                const auto disc = games::disc(game);
                if (!disc)
                    continue;
                std::vector<venue_posters::Venue> venues = venue_posters::venues(game);
                venues.push_back({"unknown", nullptr, nullptr});
                for (const venue_posters::Venue &v : venues)
                {
                    const char *venue = v.venue;
                    // Its art read as its poster, made from the disc's art
                    // the first time it is drawn; the art itself if that
                    // cannot be.
                    const textures::Source art = artOf(game, *disc, v.art);
                    textures::addEdit(textures::add(art),
                                      [g = std::string(game), v = std::string(venue)](TextureData &data)
                                      {
                                          if (const auto made = venue_posters::poster(g, v))
                                              data = *made;
                                      });
                    const std::string name = std::string("ghc_venue_") + game + "_" + venue;
                    milo::Bytes ownMat = mat;
                    if (!milo::replaceStr(ownMat, kTex, name + ".tex"))
                        return false;
                    // A portrait's Mat is ink, its colour black, at 17 in
                    // Mat 27 (r g b, content/outfits.cpp): the venue's own.
                    if (ownMat.size() >= 29u)
                        for (size_t at : {17u, 21u, 25u})
                        {
                            const float one = 1.0f;
                            std::memcpy(ownMat.data() + at, &one, 4u);
                        }
                    const auto ownMesh = poster_meshes::meshBody(mesh, kMat, picture, name + ".mesh", name + ".mat");
                    if (!ownMesh)
                        return false;
                    milo::add(scene, "Tex", name + ".tex", poster_meshes::texOf(tex, art));
                    milo::add(scene, "Mat", name + ".mat", std::move(ownMat));
                    milo::add(scene, "Mesh", name + ".mesh", *ownMesh);
                    if (milo::Bytes *group = milo::body(scene, "Group", kGroup))
                        milo::addAfter(*group, kMesh, name + ".mesh");
                }
            }
            return true;
        }

        // The outfit picker's arrow (cs_arrow.mesh: a picture 200 across from
        // its point, x 0, its head the picture's first kHead) again in two:
        // its head, and its bar from the head's end, which the hub stretches
        // to each row's words and no further (quickplay_setup.dta
        // ghc_qp_point). Drawn by no group until the hub puts them in its
        // rows' one.
        constexpr float kArrowLong = 200.0f, kArrowHigh = 32.0f, kHead = 0.16f;

        bool addArrow(milo::Dir &scene)
        {
            const milo::Bytes *like = milo::body(scene, "Mesh", "cs_arrow.mesh");
            if (!like)
                return false;
            const auto piece = [&](const std::string &name, float u0, float u1)
            {
                const float x = kArrowLong * (u1 - u0);
                poster_meshes::Mesh m;
                m.verts = {0, -kArrowHigh, 0, 0, 0, 1, u0, 1, x, -kArrowHigh, 0, 0, 0, 1, u1, 1,
                           x, kArrowHigh,  0, 0, 0, 1, u1, 0, 0, kArrowHigh,  0, 0, 0, 1, u0, 0};
                m.faces = {0, 1, 2, 3, 0, 2};
                const auto body = poster_meshes::meshBody(*like, "cs_arrow.mat", m, name, "cs_arrow.mat");
                if (body)
                    milo::add(scene, "Mesh", name, *body);
                return body.has_value();
            };
            return piece("ghc_arrow_head.mesh", 0.0f, kHead) && piece("ghc_arrow_bar.mesh", kHead, 1.0f);
        }
    }

    void install()
    {
        venue_posters::prepare();
        outfits::pickerScene(addVenues);
        outfits::pickerScene(addArrow);
    }
}
