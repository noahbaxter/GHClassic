#include "gh1/guitars.h"

#include "disc/ark.h"
#include "formats/dtb.h"
#include "gh1/rig.h"
#include "gh1/scene.h"
#include "milo/milo.h"

#include "flyingv_old_layout.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>

namespace gh2::gh1
{
    namespace
    {
        // GH1's finishes in its guitars.dta's order. Of the same model and
        // texture as GH2's (compared triangle by triangle and pixel by
        // pixel), a finish is GH2's; GH2's scenes still hold nine it does
        // not sell.
        constexpr Finish kFinishes[] = {
            {"gibson_sg_cherry", "sg_cherry", nullptr, nullptr},
            {"gibson_sg_platinum", "sg_worn", "gh1_sg", "guitar_sg_worn.mat"},
            {"gibson_sg_special", "sg_ebony", "gh1_sg_special", "guitar_sg_special.mat"},
            {"gibson_sg_flame", "sg_flames", nullptr, nullptr},
            {"gibson_lespaul_burst", "lp_cherry", nullptr, nullptr},
            {"gibson_lespaul_ebony", "lp_ebony", "lespaull", "guitar_lespaul_ebony.mat"},
            {"gibson_lespaul_gold", "lp_gold", "lespaull", "guitar_lespaul_gold.mat"},
            {"gibson_lespaul_wylde", "lp_zakk", nullptr, nullptr},
            {"gibson_flying_v_white", "fv_white", nullptr, nullptr},
            {"gibson_flying_v_red", "fv_worn", "gh1_fv", "guitar_fv_red.mat"},
            {"gibson_flying_v_v2", "fv_v2", nullptr, nullptr},
            {"gibson_flying_v_bluecrack", "fv_bluecrack", "gh1_fv_bluecrack", "guitar_fv_bluecrack.mat"},
            {"gibson_es335_triburst", "es_triburst", "gh1_es335_triburst", "guitar_es335_triburst.mat"},
            {"gibson_es335_ebony", "es_ebony", "es335_ebony_maple", "guitar_es335_ebony.mat"},
            {"gibson_es335_maple", "es_maple", nullptr, nullptr},
            {"gibson_es334_cherry", "es_cherry", nullptr, nullptr},
            {"gibson_xplorer_default", "exp_ebony", nullptr, nullptr},
            {"gibson_xplorer_white", "exp_white", nullptr, nullptr},
            {"gibson_xplorer_wood", "exp_nat", nullptr, nullptr},
            {"gibson_xplorer_sun", "exp_sun", nullptr, nullptr},
            {"gibson_firebird_default", "fb_burst", nullptr, nullptr},
            {"gibson_firebird_blue", "fb_blue", nullptr, nullptr},
            {"gibson_firebird_copper", "fb_copper", "firebird", "guitar_firebird_copper.mat"},
            {"gibson_firebird_hippie", "fb_hippie", "firebird", "guitar_firebird_hippie.mat"},
            {"gibson_lespaul_dbl_yellow", "lp_dbl_yellow", nullptr, nullptr},
            {"gibson_lespaul_dbl_white", "lp_dbl_white", nullptr, nullptr},
            {"gibson_lespaul_dbl_red", "lp_dbl_red", "gh1_lp_dbl", "guitar_lespaul_dbl_red.mat"},
            {"gibson_lespaul_dbl_custom", "lp_dbl_custom", "gh1_lespaul_dbl_custom", "guitar_lespaul_dbl_custom.mat"},
            {"gibson_corvus_yellow", "cor_yellow", nullptr, nullptr},
            {"gibson_corvus_white", "cor_white", "corvus", "guitar_corvus_white.mat"},
            {"gibson_corvus_wood", "cor_nat", nullptr, nullptr},
            {"gibson_corvus_snake", "cor_snake", "corvus", "guitar_corvus_snake.mat"},
            {"gibson_moderne_wood", "mod_nat", nullptr, nullptr},
            {"gibson_moderne_red", "mod_cherry", nullptr, nullptr},
            {"gibson_moderne_black", "mod_black", "moderne", "guitar_moderne_black.mat"},
            {"gibson_moderne_dragon", "mod_dragon", "moderne", "guitar_moderne_dragon.mat"},
            {"gibson_eds_cherry", "eds_cherry", nullptr, nullptr},
            {"gibson_eds_white", "eds_white", nullptr, nullptr},
            {"battleaxe", "battleaxe_gh1", "gh1_battleaxe", "guitar_battleaxe.mat"},
            {"grim_guitar", "scythe_gh1", "gh1_ripper", "guitar_ripper.mat"},
            {"guitar_red_octane", "guitar_red_octane", nullptr, nullptr},
        };

        // A GH1 guitar GH2 has no model of, on GH2's guitar of that body:
        // its hands' bones are that one's in GH1 too, but for the Battle
        // Axe's and the Ripper's, which bring their own.
        struct Model
        {
            const char *scene, *base, *theirs;
        };
        constexpr Model kModels[] = {
            {"gh1_sg_special", "guitar_sg", "guitar_sg_special"},
            {"gh1_fv_bluecrack", "fv_white", "guitar_fv_bluecrack"},
            {"gh1_es335_triburst", "es335_cherry", "guitar_es335_triburst"},
            {"gh1_lespaul_dbl_custom", "lp_dbl_white", "guitar_lespaul_dbl_custom"},
            {"gh1_battleaxe", "battleaxe", "guitar_battleaxe"},
            {"gh1_ripper", "scythe", "guitar_ripper"},
            // On GH1's disc and named by nothing on it (kExtra).
            {"gh1_tobac", "guitar_sg", "guitar_tobac"},
        };

        // The tobacco guitar's rig was left unfinished: its neck's bone was
        // moved to its neck, 1.4 across and 1.5 along from the Flying V's it
        // was made from, with the fret hand's bone under it held where the
        // Flying V's is, off the neck. It takes the Flying V's place under
        // the neck's bone, as every finished guitar's has.
        const char *const kUnfitted = "guitar_tobac", *const kFitted = "guitar_fv", *const kFretHand = "bone_fret_hand.mesh";
        constexpr size_t kLocal = 8u, kXfm = 48u; // a GH1 Mesh's local transform

        // A GH1 finish that is a texture on a model GH2 has: GH2's scene
        // with one more Mat, GH2's `like` naming that Tex, which comes from
        // GH1's scene unless GH2's holds it already.
        struct Texture
        {
            const char *scene, *base, *mat, *like, *tex, *theirs, *theirTex;
        };
        constexpr Texture kTextures[] = {
            {"gh1_sg", "guitar_sg", "guitar_sg_worn.mat", "guitar_sg_platinum.mat", "sg_worn.tex", "guitar_sg",
             "sg_platinum.tex"},
            {"gh1_fv", "fv_white", "guitar_fv_red.mat", "fv_white.mat", "flyingv_red.tex", "guitar_fv",
             "flyingv_red.tex"},
            {"gh1_lp_dbl", "lp_dbl_white", "guitar_lespaul_dbl_red.mat", "lp_dbl_white.mat", "dblcut_red.tex", nullptr,
             nullptr},
        };

        const char *const kTheirs = "charsys/guitars/gen/", *const kOurs = "char/og/guitars/gen/";
        const char *const kObjects = "char/gen/char_objects.dtb";
    }

    const Extra kExtra = {"tobac", {nullptr, "tobac_sunburst", "gh1_tobac", "tobacco.mat"}};

    const Unoffered kUnoffered[3] = {
        {"flying_v", {nullptr, "fv_black", "gh1_fv_black", "guitar_fv_black.mat"}, 250,
         "charsys/guitars/textures/gen/flyingv_black.bmp_ps2"},
        {"flying_v", {nullptr, "fv_blue", "gh1_fv_blue", "guitar_fv_blue.mat"}, 250,
         "charsys/guitars/textures/gen/flyingv_blue.bmp_ps2"},
        {"flying_v", {nullptr, "fv_yellow", "gh1_fv_yellow", "guitar_fv_yellow.mat"}, 250,
         "charsys/guitars/textures/gen/flyingv_yellow.bmp_ps2"},
    };

    namespace
    {
        // GH2's Flying V, whose one Tex and Mat a repainted finish is made
        // like. A bitmap is 32 bytes (its depth at 1, its width at 7 and
        // height at 9), 256 colours of four bytes, then a byte a texel; a
        // Tex ends in its bitmap.
        constexpr const char *kV = "fv_white", *kVMat = "fv_white.mat", *kVTex = "flyingv_white.tex";
        constexpr size_t kHead = 32u, kPalette = 1024u, kSide = 256u, kOldRows = 128u;

        // The old layout's bitmap as the shipped V's: the same colours, each
        // texel the old one the table names.
        Bytes repainted(const Bytes &old)
        {
            if (old.size() != kHead + kPalette + kSide * kOldRows || old[1] != 8u || sizeof kFlyingVOldLayout != kSide * kSide * 2u)
                return {};
            Bytes out(old.begin(), old.begin() + kHead + kPalette);
            out[9] = 0u;
            out[10] = 1u;
            out.resize(kHead + kPalette + kSide * kSide);
            for (size_t i = 0; i < kSide * kSide; ++i)
            {
                const size_t column = kFlyingVOldLayout[i * 2u], row = std::min<size_t>(kFlyingVOldLayout[i * 2u + 1u], kOldRows - 1u);
                out[kHead + kPalette + i] = old[kHead + kPalette + row * kSide + column];
            }
            return out;
        }
    }

    const Finish *finish(const std::string &skin)
    {
        for (const Finish &f : kFinishes)
            if (skin == f.skin)
                return &f;
        return nullptr;
    }

    std::string body(std::string gh1)
    {
        static const std::map<std::string, std::string> kBodies = {{"xplorer", "explorer"},
                                                                   {"lespaul_special", "lespaul_doublecut"},
                                                                   {"grim_guitar", "scythe"}};
        if (gh1.rfind("gibson_", 0) == 0)
            gh1 = gh1.substr(7);
        const auto it = kBodies.find(gh1);
        return it != kBodies.end() ? it->second : gh1;
    }

    void addGuitars(size_t gh1Disc)
    {
        const auto theirs = [gh1Disc](const char *scene) -> std::optional<milo::Dir>
        {
            const auto file = ark::readFile(gh1Disc, std::string(kTheirs) + scene + ".rnd_ps2");
            return file ? milo::read(*file) : std::nullopt;
        };
        const auto ours = [](const char *scene) { return load(std::string(kOurs) + scene + ".milo_ps2"); };
        // A guitar's OutfitLoader loads the scenes its type lists, and no
        // other (char_objects.dta).
        const auto objects = ark::readFile(0u, kObjects);
        auto root = objects ? dtb::raw(*objects) : std::nullopt;
        dtb::Node *listed = root ? &*root : nullptr;
        for (const char *key : {"OutfitLoader", "types", "guitar", "category", "guitars"})
            listed = listed ? const_cast<dtb::Node *>(dtb::find(*listed, key)) : nullptr;
        if (!listed)
        {
            std::cerr << "[gh1] cannot read the guitars a guitarist loads" << std::endl;
            return;
        }
        for (const Model &m : kModels)
            listed->nodes.push_back(dtb::array({dtb::symbol(m.scene)}));
        for (const Texture &t : kTextures)
            listed->nodes.push_back(dtb::array({dtb::symbol(t.scene)}));
        for (const Unoffered &u : kUnoffered)
            listed->nodes.push_back(dtb::array({dtb::symbol(u.finish.outfit)}));
        ark::addFile(kObjects, dtb::write(*root));
        for (const Model &m : kModels)
        {
            const auto base = ours(m.base);
            auto gh1 = theirs(m.theirs);
            if (!base || !gh1)
            {
                std::cerr << "[gh1] cannot read " << m.theirs << " or " << m.base << std::endl;
                continue;
            }
            if (std::string(m.theirs) == kUnfitted)
            {
                const auto fitted = theirs(kFitted);
                const auto from = fitted ? milo::find(*fitted, kFretHand) : std::nullopt;
                const auto to = milo::find(*gh1, kFretHand);
                if (from && to && fitted->bodies[*from].size() >= kLocal + kXfm && gh1->bodies[*to].size() >= kLocal + kXfm)
                    std::copy_n(fitted->bodies[*from].begin() + kLocal, kXfm, gh1->bodies[*to].begin() + kLocal);
            }
            ark::addFile(std::string(kOurs) + m.scene + ".milo_ps2", milo::write(guitar(*base, *gh1)));
        }
        for (const Texture &t : kTextures)
        {
            auto scene = ours(t.base);
            const auto gh1 = t.theirs ? theirs(t.theirs) : std::nullopt;
            const auto like = scene ? milo::find(*scene, t.like) : std::nullopt;
            const auto tex = gh1 ? milo::find(*gh1, t.theirTex) : std::nullopt;
            const auto was = like ? named(scene->bodies[*like], ".tex") : std::vector<std::string>();
            if (was.empty() || (t.theirs && !tex))
            {
                std::cerr << "[gh1] cannot build " << t.scene << std::endl;
                continue;
            }
            if (tex)
                milo::add(*scene, "Tex", t.tex, gh1->bodies[*tex]);
            milo::add(*scene, "Mat", t.mat, renamed(scene->bodies[*like], was[0], t.tex));
            ark::addFile(std::string(kOurs) + t.scene + ".milo_ps2", milo::write(*scene));
        }
        for (const Unoffered &u : kUnoffered)
        {
            auto scene = ours(kV);
            const auto like = scene ? milo::find(*scene, kVMat) : std::nullopt;
            const auto tex = scene ? milo::find(*scene, kVTex) : std::nullopt;
            const auto old = ark::readFile(gh1Disc, u.file);
            const Bytes bitmap = old ? repainted(*old) : Bytes();
            if (!like || !tex || bitmap.empty() || scene->bodies[*tex].size() < bitmap.size())
            {
                std::cerr << "[gh1] cannot build " << u.finish.outfit << std::endl;
                continue;
            }
            const std::string name = std::string(u.finish.key) + ".tex";
            Bytes body(scene->bodies[*tex].begin(), scene->bodies[*tex].end() - static_cast<std::ptrdiff_t>(bitmap.size()));
            body.insert(body.end(), bitmap.begin(), bitmap.end());
            milo::add(*scene, "Tex", name, std::move(body));
            milo::add(*scene, "Mat", u.finish.mat, renamed(scene->bodies[*like], kVTex, name));
            ark::addFile(std::string(kOurs) + u.finish.outfit + ".milo_ps2", milo::write(*scene));
        }
    }
}
