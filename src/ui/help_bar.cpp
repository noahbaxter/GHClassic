#include "ui/help_bar.h"

#include "disc/ark.h"
#include "dta.h"
#include "gh1/scene.h"
#include "guest.h"
#include "helpbar_glyphs.h"
#include "hook.h"
#include "host/devices.h"
#include "milo/milo.h"
#include "render/texture_store.h"
#include "script.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;

        // helpbar.milo draws a fret with hb_fret<n>.mat over hb_fret<n>.tex,
        // a 32x32 8-bit bitmap. Green and red are shaded art; yellow is a
        // flat placeholder no screen shows, and blue and orange are missing.
        // Those three are green or red with the hue turned.
        struct Recolor
        {
            int fret, from;
            float hue, value; // degrees, and a brightness scale
        };
        constexpr Recolor kRecolors[] = {
            {3, 2, 55.0f, 1.35f},
            {4, 1, 215.0f, 1.1f},
            {5, 2, 28.0f, 1.2f},
        };

        void recolor(uint8_t *rgb, float hue, float value)
        {
            const float r = rgb[0] / 255.0f, g = rgb[1] / 255.0f, b = rgb[2] / 255.0f;
            const float max = std::max({r, g, b}), min = std::min({r, g, b});
            const float s = max > 0.0f ? (max - min) / max : 0.0f;
            if (s <= 0.15f)
                return; // the grey rim and highlights
            const float v = std::min(1.0f, max * value), c = v * s;
            const float h = hue / 60.0f;
            const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
            const int sector = static_cast<int>(h) % 6;
            const float out[6][3] = {{c, x, 0}, {x, c, 0}, {0, c, x}, {0, x, c}, {x, 0, c}, {c, 0, x}};
            for (int i = 0; i < 3; ++i)
                rgb[i] = static_cast<uint8_t>(std::lround((out[sector][i] + v - c) * 255.0f));
        }

        // The one length-prefixed string `from` in a body, as `to`.
        bool rename(milo::Bytes &body, const std::string &from, const std::string &to)
        {
            for (size_t o = 0u; o + 4u + from.size() <= body.size(); ++o)
            {
                if (milo::u32(body, o) != from.size() || std::memcmp(body.data() + o + 4u, from.data(), from.size()) != 0)
                    continue;
                milo::Bytes out(body.begin(), body.begin() + static_cast<std::ptrdiff_t>(o));
                milo::putStr(out, to);
                out.insert(out.end(), body.begin() + static_cast<std::ptrdiff_t>(o + 4u + from.size()), body.end());
                body = std::move(out);
                return true;
            }
            return false;
        }

        // The labels' sheet (tools/procedural/helpbar_glyphs.py, whose
        // layout this is): kCell-pixel cells, kCols to a row, each label in
        // kLabels' order, white to lay over a gem; then a row holding the
        // strum bar's stand-ins, a pad's d-pad and the keyboard's arrow
        // keys, each kStrumWide by kCell / 2.
        constexpr int kCell = 96, kCols = 16, kStrumWide = 192;
        const char *const kLabels[] = {
            "A",     "B",      "X",     "Y",     "LB",   "RB",       "LT",     "RT",    "LS",      "RS",  "L1",
            "R1",    "L2",     "R2",    "L3",    "R3",   "CROSS",    "CIRCLE", "SQUARE", "TRIANGLE", "UP", "DOWN",
            "LEFT",  "RIGHT",  "START", "BACK",  "SELECT", "OPTIONS", "ENTER", "ESC",   "TAB",     "SPACE", "SHIFT",
            "0",     "1",      "2",     "3",     "4",    "5",        "6",      "7",     "8",       "9",   "C",
            "D",     "E",      "F",     "G",     "H",    "I",        "J",      "K",     "L",       "M",   "N",
            "O",     "P",      "Q",     "R",     "S",    "T",        "U",      "V",     "W",       "Z",
        };
        constexpr int kLabelCount = static_cast<int>(sizeof kLabels / sizeof kLabels[0]);
        constexpr int kStrumRow = (kLabelCount + kCols - 1) / kCols;

        // Each glyph the help bar can show in a slot: the disc's own fret
        // gem or start button with a label on it, as Tex
        // ghc_hb_<slot>_<label>.tex. The strum bar's are
        // ghc_hb_strum_pad.tex and _keys.tex.
        struct Slot
        {
            const char *name; // its Mat and Tex are hb_<name>
            input::Action action;
        };
        constexpr Slot kSlots[] = {
            {"fret1", input::kGreen}, {"fret2", input::kRed},    {"fret3", input::kYellow},
            {"fret4", input::kBlue},  {"fret5", input::kOrange}, {"start", input::kStart},
        };

        // A control as the Controllers screen names it (host/devices.h), as
        // a label's index; -1 for one with none, which shows the plain gem.
        int labelOf(const std::string &shown)
        {
            static const std::map<std::string, std::string> kNamed = {
                {"Cross", "CROSS"},       {"Circle", "CIRCLE"},     {"Square", "SQUARE"},   {"Triangle", "TRIANGLE"},
                {"D-pad Up", "UP"},       {"D-pad Down", "DOWN"},   {"D-pad Left", "LEFT"}, {"D-pad Right", "RIGHT"},
                {"L Stick Press", "LS"},  {"R Stick Press", "RS"},  {"Return", "ENTER"},    {"Keypad Enter", "ENTER"},
                {"Escape", "ESC"},        {"Left Shift", "SHIFT"},  {"Right Shift", "SHIFT"},
            };
            const auto named = kNamed.find(shown);
            std::string label = named != kNamed.end() ? named->second : shown;
            for (char &c : label)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            for (int i = 0; i < kLabelCount; ++i)
                if (label == kLabels[i])
                    return i;
            return -1;
        }

        // `data`, the sheet, as one glyph: the disc's gem brought up to
        // kCell, sampled between its pixels as the GPU draws it at its own
        // size, and the label in cell `label` laid over it, `down` pixels
        // lower, `dark` with its colours turned for the white start button.
        void glyph(TextureData &data, const TextureData &gem, int label, int down, bool dark)
        {
            std::vector<uint8_t> out(static_cast<size_t>(kCell) * kCell * 4u);
            const auto texel = [&](int x, int y, int c)
            {
                x = std::clamp(x, 0, static_cast<int>(gem.width) - 1);
                y = std::clamp(y, 0, static_cast<int>(gem.height) - 1);
                return static_cast<float>(gem.rgba[(static_cast<size_t>(y) * gem.width + x) * 4u + c]);
            };
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    uint8_t *to = out.data() + (static_cast<size_t>(y) * kCell + x) * 4u;
                    const float u = (x + 0.5f) * gem.width / kCell - 0.5f, v = (y + 0.5f) * gem.height / kCell - 0.5f;
                    const int u0 = static_cast<int>(std::floor(u)), v0 = static_cast<int>(std::floor(v));
                    const float fu = u - u0, fv = v - v0;
                    for (int c = 0; c < 4; ++c)
                        to[c] = static_cast<uint8_t>(std::lround(
                            (texel(u0, v0, c) * (1 - fu) + texel(u0 + 1, v0, c) * fu) * (1 - fv) +
                            (texel(u0, v0 + 1, c) * (1 - fu) + texel(u0 + 1, v0 + 1, c) * fu) * fv));
                    const int ly = y - down;
                    if (ly < 0 || ly >= kCell)
                        continue;
                    const size_t at = (static_cast<size_t>(label / kCols * kCell + ly) * data.width +
                                       static_cast<size_t>(label % kCols * kCell + x)) *
                                      4u;
                    const uint8_t *top = data.rgba.data() + at;
                    const float a = top[3] / 255.0f, under = to[3] / 255.0f * (1.0f - a), both = a + under;
                    for (int c = 0; c < 3 && both > 0.0f; ++c)
                        to[c] = static_cast<uint8_t>(
                            std::lround(((dark ? 255 - top[c] : top[c]) * a + to[c] * under) / both));
                    to[3] = static_cast<uint8_t>(std::lround(both * 255.0f));
                }
            data.width = data.height = static_cast<uint32_t>(kCell);
            data.rgba = std::move(out);
            data.mips.clear();
        }

        // `data`, the sheet, as one of the strum bar's stand-ins.
        void strum(TextureData &data, int which)
        {
            std::vector<uint8_t> out(static_cast<size_t>(kStrumWide) * (kCell / 2) * 4u);
            for (int y = 0; y < kCell / 2; ++y)
                std::memcpy(out.data() + static_cast<size_t>(y) * kStrumWide * 4u,
                            data.rgba.data() +
                                (static_cast<size_t>(kStrumRow * kCell + y) * data.width + which * kStrumWide) * 4u,
                            static_cast<size_t>(kStrumWide) * 4u);
            data.width = static_cast<uint32_t>(kStrumWide);
            data.height = static_cast<uint32_t>(kCell / 2);
            data.rgba = std::move(out);
            data.mips.clear();
        }

        // Each slot's glyphs, and the strum bar's, into helpbar.milo: a Tex
        // like the slot's own, a stand-in whose pixels are put together
        // from its gem and the sheet when first drawn. A label sits on a
        // fret's face, below its middle for the round top; on the start
        // button's middle.
        void addGlyphs(milo::Dir &dir)
        {
            textures::addBundled("helpbar/glyphs", kHelpbarGlyphs, sizeof kHelpbarGlyphs);
            const auto add = [&](const std::string &like, const std::string &name, uint32_t wide, uint32_t tall,
                                 std::function<void(TextureData &)> edit)
            {
                const milo::Bytes *own = milo::body(dir, "Tex", like);
                if (!own || milo::body(dir, "Tex", name))
                    return;
                const uint32_t id = textures::add({textures::kBundled, "helpbar/glyphs", name, false});
                textures::addEdit(id, std::move(edit));
                milo::add(dir, "Tex", name, gh1::texWith(*own, textures::standIn(id, wide, tall), true));
            };
            for (const Slot &slot : kSlots)
            {
                const std::string own = std::string("hb_") + slot.name + ".tex";
                const milo::Bytes *body = milo::body(dir, "Tex", own);
                const std::shared_ptr<const TextureData> gem =
                    body ? textures::decodeTex(*body, dir.version, true) : nullptr;
                if (!gem)
                {
                    std::cerr << "[help_bar] cannot read " << own << std::endl;
                    continue;
                }
                const bool start = slot.action == input::kStart;
                for (int label = 0; label < kLabelCount; ++label)
                    add(own, std::string("ghc_hb_") + slot.name + "_" + std::to_string(label) + ".tex", 32u, 32u,
                        [gem, label, start](TextureData &data)
                        { glyph(data, *gem, label, start ? 0 : kCell * 6 / 100, start); });
            }
            for (int i = 0; i < 2; ++i)
                add("hb_strum.tex", i == 0 ? "ghc_hb_strum_pad.tex" : "ghc_hb_strum_keys.tex", 64u, 16u,
                    [i](TextureData &data) { strum(data, i); });
        }

        // Each slot's Tex for what P1's menus are read from: a pad's or the
        // keyboard's controls on the gems, a guitar's own gems as GH2 drew
        // them, every fret its colour.
        std::array<std::string, 7> glyphsFor(const input::MenuControls &menu)
        {
            std::array<std::string, 7> tex;
            const bool labelled = menu.kind == input::DeviceKind::kGamepad || menu.kind == input::DeviceKind::kKeyboard;
            for (size_t i = 0; i < std::size(kSlots); ++i)
            {
                const int label = labelled ? labelOf(menu.shown[kSlots[i].action]) : -1;
                tex[i] = label < 0 ? std::string("hb_") + kSlots[i].name + ".tex"
                                   : std::string("ghc_hb_") + kSlots[i].name + "_" + std::to_string(label) + ".tex";
            }
            tex[6] = menu.kind == input::DeviceKind::kGamepad    ? "ghc_hb_strum_pad.tex"
                     : menu.kind == input::DeviceKind::kKeyboard ? "ghc_hb_strum_keys.tex"
                                                                  : "hb_strum.tex";
            return tex;
        }

        // The slots' Mats set to them, when they change or `again` (the bar
        // laid out anew, which a scene loaded again would put back).
        std::array<std::string, 7> s_shown;
        void showGlyphs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, bool again)
        {
            const std::array<std::string, 7> tex = glyphsFor(input::menuControls());
            if (tex == s_shown && !again)
                return;
            s_shown = tex;
            std::string set = "{if {exists helpbar} {do";
            for (size_t i = 0; i < tex.size(); ++i)
                set += std::string(" {{helpbar find hb_") + (i < std::size(kSlots) ? kSlots[i].name : "strum") +
                       ".mat} set diffuse_tex {helpbar find " + tex[i] + "}}";
            set += "}}";
            const R5900Context saved = *ctx;
            script::run(rdram, ctx, runtime, set);
            *ctx = saved;
        }

        struct PollTag;
        void onUiPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            showGlyphs(rdram, ctx, runtime, false);
        }

        void buildIcons()
        {
            const std::string path = "ui/gen/helpbar.milo_ps2";
            const auto file = ark::readFile(path);
            const auto raw = file ? milo::inflate(*file) : std::nullopt;
            auto dir = raw ? milo::parse(*raw) : std::nullopt;
            if (!dir)
            {
                std::cerr << "[help_bar] cannot read " << path << std::endl;
                return;
            }
            const auto body = [&](const std::string &cls, const std::string &name) { return milo::body(*dir, cls, name); };
            // The help bar's box is black and half clear, its middle
            // (help_box_mid) and its round end (help_box_corner). Each again
            // in white and solid, for a box of any colour (box.dta): a Mat's
            // colour is all its own over white.
            for (const std::string part : {"mid", "corner"})
            {
                const std::string from = "help_box_" + part, to = "ghc_box_" + part;
                const milo::Bytes *texFrom = body("Tex", from + ".tex"), *matFrom = body("Mat", from + ".mat");
                if (!texFrom || !matFrom)
                {
                    std::cerr << "[help_bar] no " << from << std::endl;
                    return;
                }
                // Its bitmap's palette as a fret's below, of 16 or 256.
                milo::Bytes tex = *texFrom, mat = *matFrom;
                const std::string png = ".png";
                const auto at = std::search(tex.begin(), tex.end(), png.begin(), png.end());
                const size_t palette = static_cast<size_t>(at - tex.begin()) + png.size() + 9u + 32u;
                if (at == tex.end() || palette > tex.size() || (tex[palette - 31u] != 4u && tex[palette - 31u] != 8u) ||
                    palette + (4u << tex[palette - 31u]) > tex.size())
                {
                    std::cerr << "[help_bar] " << from << ".tex is not a paletted bitmap" << std::endl;
                    return;
                }
                // Alpha is the PS2's, 128 for all there.
                for (size_t c = 0u; c < (1u << tex[palette - 31u]); ++c)
                {
                    uint8_t *rgba = tex.data() + palette + 4u * c;
                    rgba[0] = rgba[1] = rgba[2] = 255u;
                    rgba[3] = static_cast<uint8_t>(std::min(128, rgba[3] * 2));
                }
                rename(mat, from + ".tex", to + ".tex");
                if (!body("Tex", to + ".tex"))
                {
                    milo::add(*dir, "Tex", to + ".tex", std::move(tex));
                    milo::add(*dir, "Mat", to + ".mat", std::move(mat));
                }
            }
            for (const Recolor &r : kRecolors)
            {
                const std::string from = "hb_fret" + std::to_string(r.from), to = "hb_fret" + std::to_string(r.fret);
                const milo::Bytes *texFrom = body("Tex", from + ".tex"), *matFrom = body("Mat", from + ".mat");
                if (!texFrom || !matFrom)
                {
                    std::cerr << "[help_bar] no " << from << std::endl;
                    return;
                }
                // Tex (rev 10): width, height, bpp, its source image's path,
                // then 9 bytes, then the bitmap: a 32-byte header and the
                // palette, 256 RGBA entries.
                milo::Bytes tex = *texFrom, mat = *matFrom;
                const std::string image = "../image/helpbar_fret" + std::to_string(r.from) + ".png";
                const auto at = std::search(tex.begin(), tex.end(), image.begin(), image.end());
                const size_t palette = static_cast<size_t>(at - tex.begin()) + image.size() + 9u + 32u;
                if (at == tex.end() || tex[palette - 31u] != 8u || palette + 1024u > tex.size())
                {
                    std::cerr << "[help_bar] " << from << ".tex is not an 8-bit bitmap" << std::endl;
                    return;
                }
                for (size_t c = 0u; c < 256u; ++c)
                    recolor(tex.data() + palette + 4u * c, r.hue, r.value);
                rename(tex, image, "../image/helpbar_fret" + std::to_string(r.fret) + ".png");
                rename(mat, from + ".tex", to + ".tex");
                milo::Bytes *texTo = body("Tex", to + ".tex"), *matTo = body("Mat", to + ".mat");
                if (texTo && matTo)
                {
                    *texTo = std::move(tex);
                    *matTo = std::move(mat);
                }
                else
                {
                    milo::add(*dir, "Tex", to + ".tex", std::move(tex));
                    milo::add(*dir, "Mat", to + ".mat", std::move(mat));
                }
            }
            addGlyphs(*dir);
            ark::addFile(path, milo::write(*dir));
        }

        // HelpBarPanel::FinishLoad (0x149f40) makes max_labels (+0x48) and
        // max_buttons (+0x4c) of each, which splash.dta sets to 4: one bar
        // can now show five frets and start, each labelled, and the strum.
        struct FinishLoadTag;
        void onFinishLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t panel = GPR_U32(ctx, 4);
            store<int32_t>(rdram, panel + 0x48u, std::max(load<int32_t>(rdram, panel + 0x48u), 7));
            store<int32_t>(rdram, panel + 0x4cu, std::max(load<int32_t>(rdram, panel + 0x4cu), 6));
        }

        // HelpBarPanel::SetDisplay(DataArray *) (0x14a2f0) lays an element
        // per entry left to right from +0x70 through AddElement(element,
        // width, node, entry, Vector3 &pos): (text <token>) a label (+0x50, width +0x40),
        // (fret1|fret2|fret3|start <token>) a button (+0x60, width +0x3c)
        // then its label, (strum <token>) the strum bar (+0xa0, width +0x44)
        // then its label. Here fret4 and fret5 are buttons too.
        void setDisplay(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t panel = GPR_U32(ctx, 4), display = GPR_U32(ctx, 5);
            const uint32_t returnTo = GPR_U32(ctx, 31), sp = GPR_U32(ctx, 29);
            const uint32_t pos = sp - 0x20u;
            SET_GPR_U32(ctx, 29, pos - 0x10u);
            std::memcpy(getMemPtr(rdram, pos), getMemPtr(rdram, panel + 0x70u), 16u);
            // The elements drawn, a vector at +0x90, emptied.
            store<uint32_t>(rdram, panel + 0x94u, load<uint32_t>(rdram, panel + 0x90u));

            const auto element = [&](uint32_t vector, uint32_t i) -> uint32_t
            {
                const uint32_t at = load<uint32_t>(rdram, vector) + 4u * i;
                return at < load<uint32_t>(rdram, vector + 4u) ? load<uint32_t>(rdram, at) : 0u;
            };
            const auto addElement = [&](uint32_t added, uint32_t width, uint32_t node, uint32_t entry)
            {
                if (added == 0u)
                    return;
                ctx->f[12] = load<float>(rdram, panel + width);
                runtime->callGuestFunction(rdram, ctx, s_addresses->helpBarAddElement, {panel, added, node, entry, pos});
            };
            uint32_t labels = 0u, buttons = 0u;
            const int16_t size = load<int16_t>(rdram, display + 8u);
            for (int16_t i = 0; i < size; ++i)
            {
                const uint32_t node = load<uint32_t>(rdram, display) + 8u * static_cast<uint32_t>(i);
                if (load<uint32_t>(rdram, node + 4u) != 0x10u)
                    continue;
                const uint32_t entry = load<uint32_t>(rdram, node);
                const uint32_t first = load<uint32_t>(rdram, entry);
                if (load<uint32_t>(rdram, first + 4u) != 0x5u)
                    continue;
                const std::string key = reinterpret_cast<const char *>(getMemPtr(rdram, load<uint32_t>(rdram, first)));
                if (key == "fret1" || key == "fret2" || key == "fret3" || key == "fret4" || key == "fret5" ||
                    key == "start")
                    addElement(element(panel + 0x60u, buttons++), 0x3cu, 0u, entry);
                else if (key == "strum")
                    addElement(load<uint32_t>(rdram, panel + 0xa0u), 0x44u, 0u, entry);
                else if (key != "text")
                    continue;
                addElement(element(panel + 0x50u, labels++), 0x40u, 1u, entry);
            }
            SET_GPR_U32(ctx, 29, sp);
            showGlyphs(rdram, ctx, runtime, true);
            ctx->pc = returnTo;
        }
    }

    void installHelpBar(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        buildIcons();
        script::patchUi(kBoxDta);
        EntryHook<FinishLoadTag>::install(runtime, addresses.helpBarFinishLoad, onFinishLoad);
        EntryHook<PollTag>::install(runtime, addresses.uiManagerPoll, onUiPoll);
        runtime.replaceFunction(addresses.helpBarSetDisplay, &setDisplay);
    }
}
