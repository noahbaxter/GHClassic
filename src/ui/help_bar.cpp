#include "ui/help_bar.h"

#include "disc/ark.h"
#include "guest.h"
#include "hook.h"
#include "milo/milo.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

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
            const auto body = [&](const std::string &cls, const std::string &name) -> milo::Bytes *
            {
                const auto it = std::find(dir->entries.begin(), dir->entries.end(), std::make_pair(cls, name));
                return it == dir->entries.end() ? nullptr : &dir->bodies[static_cast<size_t>(it - dir->entries.begin())];
            };
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
            ctx->pc = returnTo;
        }
    }

    void installHelpBar(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        buildIcons();
        EntryHook<FinishLoadTag>::install(runtime, addresses.helpBarFinishLoad, onFinishLoad);
        runtime.replaceFunction(addresses.helpBarSetDisplay, &setDisplay);
    }
}
