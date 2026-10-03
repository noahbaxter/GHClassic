// The 80s guitarists are GH2's characters in new outfits, built on the same
// rigs (every anim but goth1_main is byte-identical), and they sit at GH2's
// outfit paths. Each goes in under a name of its own as one more outfit of
// its character, served from the 80s disc.
//
// An outfit loads char/<outfit>/og/<outfit>.milo (AddLoadChar, 0x128778),
// with _ui and _horse beside it, and finds its anims by a path relative to
// itself: ../../anims/ for 80s punk1, ../../../goth1/anims/ for 80s goth2.

#include "eighties.h"

#include "disc/ark.h"
#include "hook.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <string>
#include <unordered_set>

namespace gh2
{
    namespace
    {
        struct Outfit
        {
            const char *character; // in config's (characters ...)
            const char *name;      // ours
            const char *source;    // its folder and file stem on the 80s disc
            const char *label;     // the outfit picker's (sel_character.dta)
        };

        // The 80s disc names its outfits as GH2 does the ones they replaced.
        constexpr Outfit kOutfits[] = {
            {"punk", "punk3", "punk1", "'80S MOHAWK"},     {"alterna", "alterna3", "alterna1", "'80S SKULLS"},
            {"glam", "glam3", "glam1", "'80S CODPIECE"},   {"goth", "goth3", "goth2", "'80S LEATHERS"},
            {"metal", "metal3", "metal1", "'80S SHIRT"},   {"grim", "grim2", "grim", "'80S"},
        };

        std::string s_script;
        std::string s_loadOrder; // pushed onto each preview panel's load_order
        bool s_added = false;
        std::unordered_set<uint32_t> s_panelDefs;

        // CharsysPanel::SetTypeDef sizes the preview's model cache from
        // (load_order ...), ui.dta's LOAD_CHARACTERS, and an outfit outside
        // it never loads: the door says Loading. They join the list first.
        struct PanelDefTag;
        void onPanelDef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t def = GPR_U32(ctx, 5);
            if (def == 0u || !s_panelDefs.insert(def).second)
                return;
            const R5900Context saved = *ctx;
            script::setVariable(rdram, ctx, runtime, "ghc_def", {def, script::kArray});
            script::run(rdram, ctx, runtime, s_loadOrder);
            script::setVariable(rdram, ctx, runtime, "ghc_def", {0u, script::kInt});
            *ctx = saved;
        }

        // ProfileState::InitChars makes the profile's outfit items, unlocked
        // unless the store sells them (AddCharItem, 0x13ac30), from config's
        // characters, so the outfits join that list before its first run.
        struct InitCharsTag;
        void onInitChars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_added)
                return;
            s_added = true;
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, s_script);
            *ctx = saved;
        }
    }

    void installEighties(PS2Runtime &runtime, const Addresses &addresses)
    {
        const auto disc = ark::discWithSerial("SLUS_215.86");
        if (!disc)
            return;
        for (const Outfit &outfit : kOutfits)
        {
            const std::string name = outfit.name;
            const std::string source = outfit.source;
            ark::rename("char/" + name + "/og/gen/" + name, *disc, "char/" + source + "/og/gen/" + source);
            ark::rename("char/" + name + "/anims/", *disc, "char/" + source + "/anims/");
            s_script += std::string("{push_back {find $syscfg characters ") + outfit.character + "} (" + name +
                        " (name \"" + outfit.label + "\"))}\n";
            // The 80s outfit replaced GH2's default one, so source also names
            // the outfit it is cached beside. Panels can share one list.
            s_loadOrder += "{if {find_exists $ghc_def load_order} {do ($l {elem {find $ghc_def load_order} 1}) "
                           "{if {&& {find_elem $l " + source + "} {! {find_elem $l " + name + "}}} {push_back $l " +
                           name + "}}}}\n";
        }
        // Grim's one outfit has no name in the locale: his picker never
        // showed before he had two.
        s_script += "{push_back {find $syscfg characters grim grim} (name \"CLASSIC\")}\n";
        EntryHook<InitCharsTag>::install(runtime, addresses.profileStateInitChars, onInitChars);
        EntryHook<PanelDefTag>::install(runtime, addresses.charsysPanelSetTypeDef, onPanelDef);
    }
}
