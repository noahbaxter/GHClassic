#pragma once

// How GH1 says a widget draws.

#include "formats/dtb.h"
#include "gh1/scene.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace gh2::gh1
{
    // What a widget takes of a Text 15's own fields (its font, alignment,
    // text, colour, wrap width, leading, fixed length, italics and size).
    struct Template
    {
        std::string font;
        float color[3] = {}, wrap = 0.0f, size = 30.0f;
    };

    std::optional<Template> textTemplate(const Bytes &b);

    // A font as GH2's widget type of it (ui_objects.dta): GH2 has every
    // GH1 font texture as a scene of its own, and GH1's other fonts are
    // those textures again.
    std::string typeOf(const std::string &font);

    // What GH1 has to say how a widget draws: its styles by class
    // (ghui/config.dta's components), the Text each can name
    // (ghui/resources.rnd) and its strings (ghui/eng/locale.dta).
    struct Look
    {
        dtb::Node config;
        milo::Dir resources;
        std::map<std::string, Template> templates;
        std::map<std::string, float> kerning; // by font
        std::map<std::string, std::string> strings;

        // BandTextComp lays its text out with its own kerning in place
        // of the font's (SetLocalizedText, 0x29aad8).
        float kerningOf(const std::string &font) const
        {
            const auto it = kerning.find(font);
            return it != kerning.end() ? it->second : 0.0f;
        }

        const dtb::Node *style(const char *cls, const std::string &name) const
        {
            const dtb::Node *components = dtb::find(config, "components");
            const dtb::Node *of = components ? dtb::find(*components, cls) : nullptr;
            const dtb::Node *styles = of ? dtb::find(*of, "styles") : nullptr;
            return styles ? dtb::find(*styles, name) : nullptr;
        }
    };

    // A style's text colour in that state, or its one colour.
    bool colorOf(const dtb::Node *style, const char *state, float out[3]);

    // That disc's look, its scripts read into `macros`.
    std::optional<Look> lookOf(size_t disc, dtb::Macros &macros);
}
