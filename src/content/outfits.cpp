#include "content/outfits.h"

#include "content/campaigns.h"
#include "content/games.h"
#include "disc/ark.h"
#include "guest.h"
#include "hook.h"
#include "milo/milo.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace gh2::outfits
{
    namespace
    {
        struct Outfit
        {
            std::string game, own, character, outfit, beside, label;
        };
        std::vector<Outfit> s_outfits;

        // The one outfit of a GH2 character that had only one, which the
        // locale never names: its picker never showed.
        const std::map<std::string, std::string> kOwnNames = {
            {"funk1", "VELVET"},
            {"classic", "YANK"},
            {"grim", "SLAYIN'"},
        };
        bool s_configured = false;
        std::unordered_set<uint32_t> s_panelDefs;
        std::vector<std::pair<uint32_t, uint32_t>> s_panels; // each preview panel and its type definition

        // Run on config before the profiles are made: each outfit of another
        // game than `home`, the one being played, where config has its
        // character and the outfit it sits beside. (extra <game> <own name>)
        // marks it as no part of the career, there once its own game's
        // career has it (sel_character.dta).
        std::string configText(const std::string &home)
        {
            std::string text;
            std::set<std::string> labelled;
            for (const Outfit &o : s_outfits)
            {
                if (o.game == home)
                    continue;
                const std::string character = "{find $syscfg characters " + o.character + "}";
                std::string add = "{push_back " + character + " (" + o.outfit + " (name \"" + o.label + "\") (extra " +
                                  o.game + " " + o.own + "))}";
                if (const auto own = kOwnNames.find(o.character); own != kOwnNames.end() && labelled.insert(o.character).second)
                    add += " {if {! {find_exists " + character + " " + o.character + " name}} {push_back {find " +
                           character + " " + o.character + "} (name \"" + own->second + "\")}}";
                text += "{if {&& {find_exists $syscfg characters " + o.character + " " + o.beside + "} {! {find_exists " +
                        character + " " + o.outfit + "}}} " + add + "}\n";
            }
            return text;
        }

        // Run on each preview panel's type definition. Panels can share one
        // list, so only once, and only where beside is. Not the store's
        // (store TRUE), which shows only what it sells.
        std::string loadOrderText()
        {
            std::string text;
            for (const Outfit &o : s_outfits)
                if (o.game != campaigns::active())
                    text += "{if {&& {find_exists $ghc_def load_order} {! {find_exists $ghc_def store}}} "
                            "{do ($l {elem {find $ghc_def load_order} 1}) "
                            "{if {&& {find_elem $l " + o.beside + "} {! {find_elem $l " + o.outfit + "}}} {push_back $l " +
                            o.outfit + "}}}}\n";
            return text;
        }

        // ProfileState::InitChars makes the profile's outfit items, unlocked
        // unless the store sells them (AddCharItem, 0x13ac30), from config's
        // characters, so the outfits join that list before its first run.
        struct InitCharsTag;
        void onInitChars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (s_configured)
                return;
            s_configured = true;
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, configText(campaigns::active()));
            *ctx = saved;
        }

        // CharsysPanel::SetTypeDef sizes the preview's model cache from
        // (load_order ...), ui.dta's LOAD_CHARACTERS, and an outfit outside
        // it never loads: the door says Loading. They join the list first.
        struct PanelDefTag;
        void onPanelDef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t def = GPR_U32(ctx, 5);
            if (s_outfits.empty() || def == 0u || !s_panelDefs.insert(def).second)
                return;
            s_panels.emplace_back(GPR_U32(ctx, 4), def);
            const R5900Context saved = *ctx;
            script::setVariable(rdram, ctx, runtime, "ghc_def", {def, script::kArray});
            script::run(rdram, ctx, runtime, loadOrderText());
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

        // CharsysPanel::NextCharacter(index, direction, Symbol &outfit)
        // (0x142c70) steps through the models until one is valid
        // (ValidChar, 0x142bc0) and another character's than `index`'s, and
        // never ends where none is: the 80s store sells one character. There
        // the model it started on is the answer.
        PS2Runtime::RecompiledFunction s_nextCharacter = nullptr;
        void nextCharacter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const R5900Context saved = *ctx;
            const uint32_t panel = GPR_U32(ctx, 4), start = GPR_U32(ctx, 5), out = GPR_U32(ctx, 7);
            const uint32_t returnTo = GPR_U32(ctx, 31);
            const uint32_t index = GPR_U32(ctx, 29) - 16u;
            SET_GPR_U32(ctx, 29, index - 16u);
            const auto characterOf = [&](uint32_t outfit)
            {
                return static_cast<uint32_t>(runtime->callGuestFunction(
                    rdram, ctx, s_addresses->playerConfigCharacterOfOutfit, {outfit, index}));
            };
            // Models at +0x5c, 0x1c each, named at +0x00.
            const uint32_t models = load<uint32_t>(rdram, panel + 0x5cu);
            const uint32_t count = (load<uint32_t>(rdram, panel + 0x60u) - models) / 0x1cu;
            bool other = count == 0u || start >= count;
            const uint32_t own = other ? 0u : load<uint32_t>(rdram, models + start * 0x1cu);
            const uint32_t character = other ? 0u : characterOf(own);
            for (uint32_t m = 0u; m < count && !other; ++m)
            {
                const uint32_t outfit = load<uint32_t>(rdram, models + m * 0x1cu);
                other = characterOf(outfit) != character &&
                        runtime->callGuestFunction(rdram, ctx, s_addresses->charsysPanelValidChar, {panel, outfit}) != 0u;
            }
            *ctx = saved;
            if (other)
                return s_nextCharacter(rdram, ctx, runtime);
            store<uint32_t>(rdram, out, own);
            SET_GPR_U32(ctx, 2, start);
            ctx->pc = returnTo;
        }
    }

    void add(const std::string &game, const std::string &own, const std::string &character, const std::string &outfit,
             const std::string &beside, const std::string &label)
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

        s_outfits.push_back({game, own, character, outfit, beside, label});
    }

    void addTo(const script::Call &call, const std::string &home)
    {
        script::run(call.rdram, call.ctx, call.runtime, configText(home));
    }

    void preview(const script::Call &call, const std::vector<std::string> &names)
    {
        std::string text = "{if {&& {find_exists $ghc_def load_order} {! {find_exists $ghc_def store}}} "
                           "{do ($l {elem {find $ghc_def load_order} 1})";
        for (const std::string &name : names)
            text += " {if {! {find_elem $l " + name + "}} {push_back $l " + name + "}}";
        text += "}}";
        for (const uint32_t def : s_panelDefs)
        {
            script::setVariable(call.rdram, call.ctx, call.runtime, "ghc_def", {def, script::kArray});
            script::run(call.rdram, call.ctx, call.runtime, text);
        }
        script::setVariable(call.rdram, call.ctx, call.runtime, "ghc_def", {0u, script::kInt});
        // The model cache is made from the list as SetTypeDef finds it, so
        // each panel takes its definition again.
        for (const auto &[panel, def] : s_panels)
            call.runtime->callGuestFunction(call.rdram, call.ctx, s_addresses->charsysPanelSetTypeDef, {panel, def});
    }

    std::vector<std::string> all()
    {
        std::vector<std::string> out;
        for (const Outfit &o : s_outfits)
            out.push_back(o.outfit);
        return out;
    }

    std::optional<std::pair<std::string, std::string>> ownOf(const std::string &outfit)
    {
        for (const Outfit &o : s_outfits)
            if (o.outfit == outfit)
                return std::make_pair(o.game, o.own);
        return std::nullopt;
    }

    std::optional<std::string> named(const std::string &game, const std::string &own)
    {
        for (const Outfit &o : s_outfits)
            if (o.game == game && o.own == own)
                return o.outfit;
        return std::nullopt;
    }

    void campaignChanged()
    {
        s_configured = false;
        s_panelDefs.clear();
        s_panels.clear();
        s_characterOf.clear();
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

    void portrait(const std::string &outfit, bool colour,
                  std::function<std::vector<uint8_t>(const std::vector<uint8_t> &tex)> make)
    {
        struct Portrait
        {
            std::string outfit;
            bool colour;
            std::function<milo::Bytes(const milo::Bytes &)> make;
        };
        static std::vector<Portrait> s_portraits;
        if (s_portraits.empty())
        {
            // Quickplay's picker is GH2's scene whatever the menus, by its own
            // path (sel_character.dta), so this is no career's.
            ark::addMade("ghc/gh2/ui/gen/sel_character.milo_ps2", []() -> std::optional<ark::Made>
                         {
                             const auto gh2 = games::disc("gh2");
                             const auto file = gh2 ? ark::readFile(*gh2, "ui/gen/sel_character.milo_ps2") : std::nullopt;
                             auto scene = file ? milo::read(*file) : std::nullopt;
                             if (!scene)
                                 return std::nullopt;
                             for (const Portrait &p : s_portraits)
                             {
                                 const std::string &name = p.outfit;
                                 const auto o = std::find_if(s_outfits.begin(), s_outfits.end(),
                                                             [&](const Outfit &x) { return x.outfit == name; });
                                 const std::string own = o == s_outfits.end() ? "" : "char_" + o->character;
                                 const auto tex = milo::find(*scene, own + ".tex"), mat = milo::find(*scene, own + ".mat");
                                 milo::Bytes image = tex && mat ? p.make(scene->bodies[*tex]) : milo::Bytes();
                                 if (image.empty())
                                 {
                                     std::cerr << "[outfits] no portrait for " << name << std::endl;
                                     continue;
                                 }
                                 milo::Dir over;
                                 over.bodies = {scene->bodies[*mat]};
                                 // GH2's are ink: grey art in the Mat's black,
                                 // its colour at 17 in Mat 27 (r g b a).
                                 if (p.colour && over.bodies[0].size() >= 29u)
                                     for (size_t at : {17u, 21u, 25u})
                                     {
                                         const float one = 1.0f;
                                         std::memcpy(over.bodies[0].data() + at, &one, 4u);
                                     }
                                 milo::replacePrefix(over, own + ".tex", "portrait_" + name + ".tex");
                                 milo::add(*scene, "Tex", "portrait_" + name + ".tex", std::move(image));
                                 milo::add(*scene, "Mat", "portrait_" + name + ".mat", std::move(over.bodies[0]));
                             }
                             auto bytes = std::make_shared<milo::Bytes>(milo::write(*scene));
                             return ark::Made{static_cast<uint32_t>(bytes->size()),
                                              [bytes](uint64_t offset, uint8_t *dst, size_t n)
                                              {
                                                  if (offset >= bytes->size())
                                                      return size_t{0};
                                                  n = std::min<size_t>(n, bytes->size() - offset);
                                                  std::memcpy(dst, bytes->data() + offset, n);
                                                  return n;
                                              }};
                         });
        }
        s_portraits.push_back({outfit, colour, std::move(make)});
    }

    void install(PS2Runtime &runtime, const Addresses &addresses)
    {
        EntryHook<InitCharsTag>::install(runtime, addresses.profileStateInitChars, onInitChars);
        EntryHook<PanelDefTag>::install(runtime, addresses.charsysPanelSetTypeDef, onPanelDef);
        s_addresses = &addresses;
        EntryHook<PollCharLoadingTag>::install(runtime, addresses.charsysPanelPollCharLoading, onPollCharLoading);
        s_nextCharacter = runtime.lookupFunction(addresses.charsysPanelNextCharacter);
        runtime.replaceFunction(addresses.charsysPanelNextCharacter, &nextCharacter);
    }
}
