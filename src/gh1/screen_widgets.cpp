// GH1's labels and buttons as GH2's.

#include "gh1/screen.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gh2::gh1
{
    namespace
    {
        using milo::putStr;
        using milo::putU32;
        using milo::str;
        using Row = Screen::Row;

        // GH2's packing of a colour (pack_color): red lowest.
        uint32_t packed(const float c[3])
        {
            uint32_t out = 0u;
            for (int i = 0; i < 3; ++i)
                out |= static_cast<uint32_t>(std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f)) << (8 * i);
            return out;
        }

        // That int property of a dir's own object, set: its key as a symbol
        // node, then an int node (TypeProps::Load, 0x2c7650).
        void setProp(Bytes &root, const std::string &key, uint32_t value)
        {
            Bytes find;
            putStr(find, key);
            putU32(find, 0u);
            const auto it = std::search(root.begin(), root.end(), find.begin(), find.end());
            if (it == root.end() || static_cast<size_t>(root.end() - it) < find.size() + 4u)
                return;
            std::memcpy(&*(it + static_cast<std::ptrdiff_t>(find.size())), &value, 4u);
        }

        // A button's local at that height, and its world moved as far
        // along its View's.
        void place(Bytes &b, const Parts &p, float z)
        {
            const size_t local = p.trans.at + 4u + 44u, world = p.trans.at + 52u + 36u;
            const float by = z - f32(b, local);
            std::memcpy(b.data() + local, &z, 4u);
            for (size_t axis = 0; axis < 3u; ++axis)
            {
                const float moved = f32(b, world + 4u * axis) + by * f32(b, p.trans.at + 52u + 24u + 4u * axis);
                std::memcpy(b.data() + world + 4u * axis, &moved, 4u);
            }
        }
    }

    // A UIComponent 1 of that type (UIComponent::PreLoad, 0x20d490):
    // Hmx::Object's header (0x2c2018), a Trans 9 under its parent, a
    // Draw 3, and the components right of and below it. `flat` leaves
    // its local at its position alone. One under nothing names no parent:
    // a Trans 9 under itself is never drawn (RndTransformable::Load,
    // 0x3d72d0, takes only a Trans 8's own name for none).
    void Conversion::putComponent(Bytes &out, const Bytes &b, const std::string &n, const Parts &p,
                                  const std::string &type, bool flat)
    {
        putU32(out, 1u);
        putU32(out, 0u);
        putStr(out, type);
        out.push_back(0u);
        putU32(out, 9u);
        if (flat)
            for (int f = 0; f < 9; ++f)
                putF32(out, f % 4 == 0 ? 1.0f : 0.0f);
        put(out, b, p.trans.at + (flat ? 40u : 4u), p.trans.at + 100u);
        put(out, b, p.trans.constraint, p.trans.parent);
        const auto owner = parent.find(n);
        putStr(out, owner != parent.end() ? owner->second : std::string());
        putU32(out, 3u);
        out.push_back(shows(n, p) ? 1u : 0u);
        put(out, b, p.draw.sphere, p.draw.sphere + 16u);
        putF32(out, 0.0f);
        putStr(out, {});
        const auto below = linked.find(n);
        putStr(out, below != linked.end() && part.count(below->second) ? below->second : std::string());
    }

    // A UILabel 1's own (UILabel::PreLoad, 0x217d90): whether its text
    // is a token, and the text. A token GH1 has a string for stays one,
    // with that string; anything else is the text.
    void Conversion::putText(Bytes &out, const std::string &token)
    {
        const auto it = look.strings.find(token);
        if (it != look.strings.end())
            text[token] = it->second;
        out.push_back(it != look.strings.end() ? 1u : 0u);
        putStr(out, token);
    }

    // A LabelEx 6 (RndLabelEx::Load, GH1 0x120d10): whether it fits its
    // box, the box, its leading, alignment, jitter seed and ranges, style,
    // caps, token and wrap width. Its style's Text has its font, size and,
    // where the style has none, colour.
    bool Conversion::addLabel(const std::string &n, const Bytes &b)
    {
        const Parts &p = part[n];
        size_t o = p.rest + 33u;
        const std::string styleName = str(b, o);
        const bool caps = u8(b, o) != 0u;
        o += 1u;
        const std::string token = str(b, o);
        const float wrap = f32(b, o);
        const dtb::Node *style = look.style("labelex", styleName);
        const dtb::Node *its = style ? dtb::find(*style, "text") : nullptr;
        const auto found = look.templates.find(its && its->nodes.size() > 1u ? its->nodes[1].text : "label.txt");
        if (found == look.templates.end())
            return false;
        const Template &of = found->second;
        float color[3] = {of.color[0], of.color[1], of.color[2]};
        colorOf(style, "normal", color);
        // A BandLabel 12 (BandLabel::PreLoad, 0x28e388). A label that fits
        // takes its box's scale in place of its own
        // (RndLabelEx::JustifyText, GH1 0x121c00), where GH2's scales its
        // text under it (BandTextComp::FitText, 0x29b2c8). One with no
        // wrap width keeps its Text's (RndLabelEx::SetWrapWidth, GH1
        // 0x121168).
        const bool fit = u8(b, p.rest) != 0u;
        Bytes body;
        putU32(body, 12u);
        putU32(body, 1u);
        putComponent(body, b, n, p, typeOf(of.font), fit);
        putText(body, token);
        putU32(body, fit ? 1u : 0u);
        put(body, b, p.rest + 1u, p.rest + 33u);
        body.push_back(caps ? 1u : 0u);
        putF32(body, look.kerningOf(of.font));
        putF32(body, of.size);
        putF32(body, wrap > 0.0f ? wrap : of.wrap);
        for (const float v : color)
            putF32(body, v);
        putF32(body, 1.0f);
        milo::add(out, "BandLabel", n, std::move(body));
        return true;
    }

    // A ButtonEx 4 (RndButtonEx::Load, GH1 0x12e618): its jitter seed and
    // ranges, style, whether it fits its box, caps, the box and its token.
    // Its Text is button.txt in its style's font, in the style's colour for
    // its state (RndButtonEx::UpdateColors, GH1 0x12ef18).
    bool Conversion::addButton(const std::string &n, const Bytes &b)
    {
        const Parts &p = part[n];
        size_t o = p.rest + 16u;
        const std::string styleName = str(b, o);
        Button button;
        button.fit = u8(b, o);
        button.caps = u8(b, o + 1u) != 0u;
        button.box = o + 2u;
        o += 10u;
        button.token = str(b, o);
        const dtb::Node *style = look.style("buttonex", styleName);
        const dtb::Node *font = style ? dtb::find(*style, "font") : nullptr;
        const auto found = look.templates.find("button.txt");
        if (found == look.templates.end())
            return false;
        const Template &of = found->second;
        colorButtons(style);
        button.face = font && font->nodes.size() > 1u ? font->nodes[1].text : of.font;
        button.size = of.size;
        // Itself, then each button of GH2's script made in its likeness.
        std::vector<const Row *> made = {nullptr};
        for (const Row &r : screen.rows)
            if (r.of && n == r.of)
                made.push_back(&r);
        for (const Row *row : made)
        {
            const std::string name = row ? row->name : n;
            milo::add(out, "BandButton", name, bandButton(name, row, b, p, button));
        }
        return true;
    }

    // GH2 colours a panel's buttons alike, by its own object's properties
    // (ui_objects.dta's PanelDir GH2).
    void Conversion::colorButtons(const dtb::Node *style)
    {
        if (!colored)
        {
            static const std::pair<const char *, const char *> kStates[] = {
                {"normal_color", "normal"},
                {"focus_color", "focused"},
                {"selecting_color", "selected"},
                {"disabled_color", "disabled"},
            };
            float color[3];
            for (const auto &[prop, state] : kStates)
                if (colorOf(style, state, color))
                    setProp(out.root, prop, packed(color));
            colored = true;
        }
    }

    // A BandButton 10 (BandButton::PreLoad, 0x28b6b8), a UIButton 0
    // (0x217310). GH1's button fits its box's width alone, centred on one
    // line (RndButtonEx::JustifyText, GH1 0x12f190, and its constructor,
    // GH1 0x12dc40); GH2's fits a height too unless it is none
    // (BandTextComp::FitText, 0x29b2c8), and centres by alignment 0x22.
    Bytes Conversion::bandButton(const std::string &name, const Row *row, const Bytes &b, const Parts &p,
                                 const Button &button)
    {
        Bytes placed = b;
        if (const auto own = rows.find(name); own != rows.end())
            place(placed, p, own->second->z);
        Bytes body;
        putU32(body, 10u);
        putU32(body, 0u);
        putU32(body, 1u);
        putComponent(body, placed, name, p, typeOf(button.face), false);
        if (row)
        {
            // A token of GH2's, or text it has no string for, which reads
            // as itself.
            body.push_back(1u);
            putStr(body, row->text);
        }
        else
            putText(body, button.token);
        putU32(body, button.fit);
        putF32(body, f32(b, button.box));
        putF32(body, button.fit != 0u ? 0.0f : f32(b, button.box + 4u));
        putF32(body, 1.0f);
        putU32(body, 0x22u);
        put(body, b, p.rest, p.rest + 16u);
        body.push_back(button.caps ? 1u : 0u);
        putF32(body, look.kerningOf(button.face));
        putF32(body, button.size);
        putF32(body, 0.0f);
        return body;
    }
}
