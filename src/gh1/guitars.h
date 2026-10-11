#pragma once

// GH1's guitar finishes as finishes of GH2's guitars.
//
// Most of GH1's guitars are GH2's models of the same Gibsons, and most of
// its finishes are on GH2's disc, sold or not. The rest are built here from
// GH1's disc: a finish that is only a texture goes on GH2's model, and a
// guitar GH2 has no model of comes over whole (gh1/rig.h), as one more
// finish of GH2's guitar of that body.

#include <cstddef>
#include <optional>
#include <string>

namespace gh2::gh1
{
    // A GH1 finish in GH2's guitars config. With no outfit it is GH2's own
    // finish of that key.
    struct Finish
    {
        const char *skin;   // GH1's
        const char *key;    // GH2's, or ours
        const char *outfit; // the scene under char/og/guitars
        const char *mat;
    };

    const Finish *finish(const std::string &skin);

    // The guitar GH1's disc holds and nothing on it names
    // (guitar_tobac.rnd): a double-cut in tobacco sunburst, with no name,
    // blurb or price of GH1's. Here it is one more guitar of GH1's career,
    // a band's from the start, its name and blurb content/guitar_copy.dta's.
    struct Extra
    {
        const char *guitar;
        Finish finish;
    };
    extern const Extra kExtra;

    // Three Flying V finishes GH1's disc holds as loose textures no scene
    // draws (flyingv_black, _blue and _yellow.bmp), painted for an older
    // model of the V that only a GH2 demo disc has, in a layout the V that
    // shipped cannot wear. Each is repainted here into the shipped V's
    // layout, texel by texel, by a table of where each texel of the one
    // sits in the other (flyingv_old_layout.bin: the nearest point of the
    // old model's surface to each texel's place on the shipped one, worked
    // out once with the demo's model). Finishes of GH1's career's Flying V,
    // sold at the price of its dearest.
    struct Unoffered
    {
        const char *guitar;
        Finish finish;
        int price;
        const char *file; // on GH1's disc
    };
    extern const Unoffered kUnoffered[3];

    // A GH1 guitar's name in its career's config: its Gibsons by GH2's names
    // for the same bodies and the Grim Ripper's as GH2's, the Battle Axe
    // by its own.
    std::string body(std::string gh1);

    // The scenes those finishes name that no disc has, from GH1's disc.
    void addGuitars(size_t gh1Disc);
}
