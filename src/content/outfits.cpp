#include "content/outfits.h"

#include "disc/ark.h"
#include "guest.h"
#include "hook.h"
#include "milo/milo.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <iostream>
#include <set>
#include <unordered_map>
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

        const Addresses *s_addresses = nullptr;
        std::unordered_map<uint32_t, uint32_t> s_characterOf; // outfit Symbol to character's

        // CharsysPanel::PrioritizeModels (0x142880) ranks each placer's outfit
        // 0 and its character's other one 1, as if every character had two,
        // then the neighbours' from 2. A third outfit, unranked, loads only
        // once shown, evicting the first two. Before each loading pass every
        // outfit of a shown character ranks 1.
        struct PollCharLoadingTag;
        void onPollCharLoading(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            const R5900Context saved = *ctx;
            // CharacterOfOutfit's index goes below the hooked call's frame.
            const uint32_t index = GPR_U32(ctx, 29) - 16u;
            SET_GPR_U32(ctx, 29, index - 16u);
            auto characterOf = [&](uint32_t outfit)
            {
                const auto it = s_characterOf.find(outfit);
                if (it != s_characterOf.end())
                    return it->second;
                uint32_t character = static_cast<uint32_t>(runtime->callGuestFunction(
                    rdram, ctx, s_addresses->playerConfigCharacterOfOutfit, {outfit, index}));
                // None is the empty Symbol.
                if (character != 0u && load<uint8_t>(rdram, character) == 0u)
                    character = 0u;
                return s_characterOf[outfit] = character;
            };
            // Placers at +0x3c, 0x30 each, the outfit shown at +0x28; models
            // at +0x5c, 0x1c each, named at +0x00.
            const uint32_t models = load<uint32_t>(rdram, panel + 0x5cu);
            const uint32_t modelsEnd = load<uint32_t>(rdram, panel + 0x60u);
            for (uint32_t p = load<uint32_t>(rdram, panel + 0x3cu); p < load<uint32_t>(rdram, panel + 0x40u);
                 p += 0x30u)
            {
                const uint32_t shown = load<uint32_t>(rdram, p + 0x28u);
                const uint32_t character = shown ? characterOf(shown) : 0u;
                if (character == 0u)
                    continue;
                for (uint32_t m = models; m < modelsEnd; m += 0x1cu)
                {
                    const uint32_t outfit = load<uint32_t>(rdram, m);
                    if (outfit != shown && characterOf(outfit) == character)
                        runtime->callGuestFunction(rdram, ctx, s_addresses->charsysPanelTrySetPriority,
                                                   {panel, outfit, 1u});
                }
            }
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

    void photosFrom(const std::string &outfit, size_t disc, const std::string &source)
    {
        // Whole names: grim2's photo 0 is photo_grim20, grim's 2 photo_grim2.
        for (int i = 0; i < 4; ++i)
        {
            const std::string index = std::to_string(i) + "_keep";
            ark::rename("ui/image/og/gen/photo_" + outfit + index, disc, "ui/image/og/gen/photo_" + source + index);
        }
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
        s_addresses = &addresses;
        EntryHook<PollCharLoadingTag>::install(runtime, addresses.charsysPanelPollCharLoading, onPollCharLoading);
    }
}
