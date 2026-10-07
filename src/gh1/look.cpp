#include "gh1/look.h"

#include "disc/ark.h"

namespace gh2::gh1
{
    namespace
    {
        using milo::str;
        using milo::u32;
    }

    std::optional<Template> textTemplate(const Bytes &b)
    {
        const auto p = parts("Text", b);
        if (!p || u32(b, 0u) != 15u)
            return std::nullopt;
        Template out;
        size_t o = p->rest;
        out.font = str(b, o);
        o += 4u;
        str(b, o);
        for (int i = 0; i < 3; ++i)
            out.color[i] = f32(b, o + 4u * static_cast<size_t>(i));
        out.wrap = f32(b, o + 16u);
        out.size = f32(b, o + 32u);
        return out;
    }

    std::string typeOf(const std::string &font)
    {
        static const std::map<std::string, std::string> kOthers = {
            {"song_header", "hand"},        {"song_blurb", "hand"},     {"credit_name", "serif"},
            {"credit_title", "serif"},      {"credit_center_name", "serif"}, {"credit_center", "impactor"},
        };
        const auto it = kOthers.find(base(font));
        return it != kOthers.end() ? it->second : base(font);
    }

    bool colorOf(const dtb::Node *style, const char *state, float out[3])
    {
        const dtb::Node *colors = style ? dtb::find(*style, "colors") : nullptr;
        const dtb::Node *text = colors ? dtb::find(*colors, "text_color") : nullptr;
        if (!text || text->nodes.size() < 2u)
            return false;
        if (text->nodes[1].type == dtb::kArray)
            text = dtb::find(*text, state);
        if (!text || text->nodes.size() < 4u)
            return false;
        for (size_t i = 0; i < 3u; ++i)
            out[i] = dtb::number(text->nodes[i + 1u]).value_or(0.0f);
        return true;
    }

    std::optional<Look> lookOf(size_t disc, dtb::Macros &macros)
    {
        const dtb::Files theirs = [disc](const std::string &path) { return ark::readFile(disc, path); };
        Look look;
        const auto config = dtb::read("ghui/config.dta", macros, theirs);
        const auto strings = dtb::read("ghui/eng/locale.dta", macros, theirs);
        const auto resources = loadScene(disc, "ghui/gen/resources.rnd_ps2");
        if (!config || !strings || !resources)
            return std::nullopt;
        look.config = *config;
        look.resources = *resources;
        for (const dtb::Node &entry : strings->nodes)
            if (entry.type == dtb::kArray && entry.nodes.size() > 1u)
                look.strings[entry.nodes[0].text] = entry.nodes[1].text;
        for (size_t i = 0; i < resources->entries.size(); ++i)
            if (resources->entries[i].first == "Text")
                if (const auto t = textTemplate(resources->bodies[i]))
                    look.templates[resources->entries[i].second] = *t;
        // A Font 7 (RndFont::Load, GH1 0x1c0830): its mat, its cell, a size
        // and its kerning.
        for (size_t i = 0; i < resources->entries.size(); ++i)
            if (resources->entries[i].first == "Font")
            {
                size_t o = 4u;
                str(resources->bodies[i], o);
                look.kerning[resources->entries[i].second] = f32(resources->bodies[i], o + 12u);
            }
        return look;
    }
}
