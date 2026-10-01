#include "outfits.h"

#include "disc/ark.h"
#include "hook.h"
#include "milo/milo.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <iostream>
#include <set>
#include <unordered_set>

namespace gh2::outfits
{
    namespace
    {
        std::string s_config;    // run on config before the profiles are made
        std::string s_loadOrder; // run on each preview panel's type definition
        std::set<std::pair<std::string, std::string>> s_labelled;
        bool s_configured = false;
        std::unordered_set<uint32_t> s_panelDefs;

        // ProfileState::InitChars makes the profile's outfit items, unlocked
        // unless the store sells them (AddCharItem, 0x13ac30), from config's
        // characters, so the outfits join that list before its first run.
        struct InitCharsTag;
        void onInitChars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_configured || s_config.empty())
                return;
            s_configured = true;
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, s_config);
            *ctx = saved;
        }

        // CharsysPanel::SetTypeDef sizes the preview's model cache from
        // (load_order ...), ui.dta's LOAD_CHARACTERS, and an outfit outside
        // it never loads: the door says Loading. They join the list first.
        struct PanelDefTag;
        void onPanelDef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t def = GPR_U32(ctx, 5);
            if (s_loadOrder.empty() || def == 0u || !s_panelDefs.insert(def).second)
                return;
            const R5900Context saved = *ctx;
            script::setVariable(rdram, ctx, runtime, "ghc_def", {def, script::kArray});
            script::run(rdram, ctx, runtime, s_loadOrder);
            script::setVariable(rdram, ctx, runtime, "ghc_def", {0u, script::kInt});
            *ctx = saved;
        }
    }

    void add(const std::string &character, const std::string &outfit, const std::string &beside,
             const std::string &label)
    {
        // The picker hands the playing clips from one outfit's driver to the
        // next (CharDriver::Transfer, 0x170db0), so they must share beside's
        // clip set, as metal2 shares metal1's: the same anims, by one path.
        for (const char *suffix : {"", "_ui", "_horse"})
        {
            const std::string path = "char/" + outfit + "/og/gen/" + outfit + suffix + ".milo_ps2";
            const auto file = ark::readFile(path);
            const auto raw = file ? milo::inflate(*file) : std::nullopt;
            auto dir = raw ? milo::parse(*raw) : std::nullopt;
            if (!dir)
            {
                std::cerr << "[outfits] cannot read " << path << std::endl;
                continue;
            }
            milo::replacePrefix(*dir, "../../anims/", "../../../" + beside + "/anims/");
            ark::addFile(path, milo::write(*dir));
        }

        s_config += "{push_back {find $syscfg characters " + character + "} (" + outfit + " (name \"" + label +
                    "\"))}\n";
        // Panels can share one list, so only once, and only where beside is.
        s_loadOrder += "{if {find_exists $ghc_def load_order} {do ($l {elem {find $ghc_def load_order} 1}) "
                       "{if {&& {find_elem $l " + beside + "} {! {find_elem $l " + outfit + "}}} {push_back $l " +
                       outfit + "}}}}\n";
    }

    void label(const std::string &character, const std::string &outfit, const std::string &label)
    {
        if (!s_labelled.insert({character, outfit}).second)
            return;
        s_config += "{push_back {find $syscfg characters " + character + " " + outfit + "} (name \"" + label +
                    "\")}\n";
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<InitCharsTag>::install(runtime, addresses.profileStateInitChars, onInitChars);
        EntryHook<PanelDefTag>::install(runtime, addresses.charsysPanelSetTypeDef, onPanelDef);
    }
}
