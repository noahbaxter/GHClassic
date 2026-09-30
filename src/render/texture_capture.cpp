// Texture pixels in the engine's neutral form, taken when a texture syncs.
//
// PsTex::SyncBitmap (0x1a13a8) turns a RndTex's bitmap into GS TEX0 state
// and a VRAM upload. The bitmap is decoded to RGBA on entry instead, with
// RndBitmap's own rules: PixelOffset (0x1aed38) for where a pixel lives,
// PaletteOffset (0x1b0f08) for which CLUT entry an index means, and
// ConvertColor (0x1ae538) for the channel order. The original still runs,
// since its VRAM bookkeeping is shared with PsTex's other paths until PsTex
// is replaced whole.

#include "render/texture_capture.h"

#include "guest.h"
#include "hook.h"
#include "milo/layout.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <unordered_map>

namespace gh2
{
    namespace
    {
        const Addresses *s_addresses = nullptr;
        std::unordered_map<uint32_t, std::shared_ptr<const TextureData>> s_textures;

        struct Bitmap
        {
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t rowBytes = 0;
            uint32_t bpp = 0;
            uint32_t order = 0;
            std::vector<uint8_t> pixels;
            std::vector<uint8_t> palette;
        };

        // PixelOffset's tables, copied out of the guest on first use.
        uint8_t s_swizzle8[2][64];
        uint8_t s_swizzle4[2][128];
        bool s_haveSwizzle = false;

        std::vector<uint8_t> copyGuest(uint8_t *rdram, uint32_t address, uint32_t bytes)
        {
            std::vector<uint8_t> out(bytes);
            for (uint32_t i = 0; i < bytes; ++i)
                out[i] = load<uint8_t>(rdram, address + i);
            return out;
        }

        // RndBitmap::PixelOffset: the byte holding pixel (x, y), and for 4bpp
        // whether it is the high nibble.
        uint32_t pixelOffset(const Bitmap &b, uint32_t x, uint32_t y, bool &high)
        {
            high = (x & 1u) != 0u;
            if (!(b.order & milo::bitmap::kSwizzled) || (b.bpp != 4u && b.bpp != 8u))
                return y * b.rowBytes + ((x * b.bpp) >> 3);

            if (b.bpp == 8u)
            {
                const uint32_t base = (y >> 2) * 4u * b.rowBytes + (x >> 4) * 32u;
                uint32_t t = s_swizzle8[(y >> 2) & 1u][(y & 3u) * 16u + (x & 15u)];
                if (t >= 32u)
                    t += 2u * b.rowBytes - 32u;
                return base + t;
            }

            const uint32_t quad = (y >> 2) & 3u;
            uint32_t stripe, row, col;
            if (b.width > 128u && b.height > 128u)
            {
                stripe = (b.width >> 7) * 256u + ((b.height * 2u) & 0xe0u);
                row = (y >> 7) * 32u + ((x & 127u) >> 5) * 8u + quad * 2u;
                col = ((x >> 7) * 64u + ((y & 127u) >> 4) * 64u) * 4u;
            }
            else
            {
                stripe = b.height * 2u;
                row = (x >> 5) * 8u + quad * 2u;
                col = (y >> 4) * 32u;
            }
            const uint32_t entry = s_swizzle4[quad & 1u][(y & 3u) * 32u + (x & 31u)];
            high = (entry & 1u) != 0u;
            uint32_t t = entry >> 1;
            if (t >= 32u)
                t += stripe - 32u;
            return row * stripe + col + t;
        }

        // RndBitmap::PaletteOffset: the GS stores 8bpp CLUTs with bits 3 and
        // 4 of the index swapped.
        uint32_t paletteOffset(const Bitmap &b, uint32_t index)
        {
            if ((b.order & milo::bitmap::kPs2Alpha) && b.bpp == 8u)
            {
                const uint32_t bits = index & 0x18u;
                if (bits == 0x08u)
                    return index + 8u;
                if (bits == 0x10u)
                    return index - 8u;
            }
            return index;
        }

        uint8_t expandAlpha(const Bitmap &b, uint32_t alpha)
        {
            if (b.order & milo::bitmap::kPs2Alpha)
                alpha = std::min<uint32_t>(alpha * 255u >> 7, 255u);
            return static_cast<uint8_t>(alpha);
        }

        // RndBitmap::ConvertColor for a 4-byte palette entry or pixel.
        void convert32(const Bitmap &b, const uint8_t *src, uint8_t *out)
        {
            const bool rgba = (b.order & milo::bitmap::kRgba) != 0u;
            out[0] = rgba ? src[0] : src[2];
            out[1] = src[1];
            out[2] = rgba ? src[2] : src[0];
            out[3] = expandAlpha(b, src[3]);
        }

        void convertPixel(const Bitmap &b, uint32_t offset, bool high, uint8_t *out)
        {
            const bool rgba = (b.order & milo::bitmap::kRgba) != 0u;
            const uint8_t *src = b.pixels.data() + offset;
            switch (b.bpp)
            {
            case 4u:
            case 8u:
            {
                uint32_t index = src[0];
                if (b.bpp == 4u)
                    index = high ? index >> 4 : index & 0xfu;
                const uint32_t entry = paletteOffset(b, index) * 4u;
                if (entry + 4u <= b.palette.size())
                    convert32(b, b.palette.data() + entry, out);
                break;
            }
            case 16u:
            {
                const uint32_t v = static_cast<uint32_t>(src[0]) | static_cast<uint32_t>(src[1]) << 8;
                const uint8_t low = static_cast<uint8_t>((v & 0x1fu) << 3);
                const uint8_t mid = static_cast<uint8_t>((v & 0x3e0u) >> 2);
                const uint8_t top = static_cast<uint8_t>((v & 0x7c00u) >> 7);
                out[0] = rgba ? low : top;
                out[1] = mid;
                out[2] = rgba ? top : low;
                out[3] = (v & 0x8000u) ? 255u : 0u;
                break;
            }
            case 24u:
                out[0] = rgba ? src[0] : src[2];
                out[1] = src[1];
                out[2] = rgba ? src[2] : src[0];
                out[3] = 255u;
                break;
            case 32u:
                convert32(b, src, out);
                break;
            }
        }

        std::shared_ptr<const TextureData> decode(uint8_t *rdram, uint32_t address)
        {
            Bitmap b;
            b.width = load<uint16_t>(rdram, address + milo::bitmap::kWidth);
            b.height = load<uint16_t>(rdram, address + milo::bitmap::kHeight);
            b.rowBytes = load<uint16_t>(rdram, address + milo::bitmap::kRowBytes);
            b.bpp = load<uint8_t>(rdram, address + milo::bitmap::kBpp);
            b.order = load<uint32_t>(rdram, address + milo::bitmap::kOrder);
            const uint32_t pixels = load<uint32_t>(rdram, address + milo::bitmap::kPixels);
            const uint32_t palette = load<uint32_t>(rdram, address + milo::bitmap::kPalette);
            if (b.width == 0u || b.height == 0u || pixels == 0u)
                return nullptr;
            if (b.bpp != 4u && b.bpp != 8u && b.bpp != 16u && b.bpp != 24u && b.bpp != 32u)
                return nullptr;
            if (b.bpp <= 8u && palette == 0u)
                return nullptr;

            b.pixels = copyGuest(rdram, pixels, b.rowBytes * b.height);
            if (b.bpp <= 8u)
                b.palette = copyGuest(rdram, palette, 4u << b.bpp);

            auto out = std::make_shared<TextureData>();
            out->width = b.width;
            out->height = b.height;
            out->rgba.assign(static_cast<size_t>(b.width) * b.height * 4u, 0u);
            for (uint32_t y = 0; y < b.height; ++y)
                for (uint32_t x = 0; x < b.width; ++x)
                {
                    bool high = false;
                    const uint32_t offset = pixelOffset(b, x, y, high);
                    if (offset + (b.bpp + 7u) / 8u <= b.pixels.size())
                        convertPixel(b, offset, high, &out->rgba[(static_cast<size_t>(y) * b.width + x) * 4u]);
                }
            return out;
        }

        // GH2's help bar icon for the yellow fret is an unfinished placeholder:
        // an opaque black ground, a flat fill and a white column down each
        // side. Vanilla never shows it (its help bars use green, red and the
        // strum); the lag screen does, so it is drawn as the green fret's icon
        // in yellow. Both are known by their pixels, the same every run.
        constexpr uint64_t kGreenFretIcon = 0x30e390c3437871eaull;
        constexpr uint64_t kYellowFretIcon = 0x0cb4603bf60186c0ull;
        std::shared_ptr<const TextureData> s_greenFretIcon;
        std::vector<uint32_t> s_yellowFretIcons; // redrawn once green is seen

        uint64_t pixelHash(const TextureData &data)
        {
            uint64_t hash = 1469598103934665603ull; // FNV-1a
            for (uint8_t c : data.rgba)
                hash = (hash ^ c) * 1099511628211ull;
            return hash;
        }

        // The green fret icon in another colour: its fill (green 139) becomes
        // `fill`, and its shading, carried by the green channel, scales it.
        std::shared_ptr<const TextureData> tinted(const TextureData &green, const float (&fill)[3])
        {
            constexpr float kGreenFill = 139.0f;
            auto out = std::make_shared<TextureData>(green);
            for (size_t i = 0; i < out->rgba.size(); i += 4u)
            {
                const float shade = out->rgba[i + 1u] / kGreenFill;
                for (size_t c = 0; c < 3u; ++c)
                    out->rgba[i + c] = static_cast<uint8_t>(std::min(255.0f, shade * fill[c]));
            }
            return out;
        }

        constexpr float kYellowFill[3] = {222.0f, 201.0f, 62.0f};

        std::shared_ptr<const TextureData> yellowed(const TextureData &green)
        {
            return tinted(green, kYellowFill);
        }

        void store(uint32_t tex, std::shared_ptr<const TextureData> data)
        {
            if (data && data->width == 32u && data->height == 32u)
            {
                const uint64_t hash = pixelHash(*data);
                if (hash == kGreenFretIcon)
                {
                    s_greenFretIcon = data;
                    for (uint32_t yellow : s_yellowFretIcons)
                        s_textures[yellow] = yellowed(*data);
                }
                else if (hash == kYellowFretIcon)
                {
                    if (std::find(s_yellowFretIcons.begin(), s_yellowFretIcons.end(), tex) == s_yellowFretIcons.end())
                        s_yellowFretIcons.push_back(tex);
                    if (s_greenFretIcon)
                        data = yellowed(*s_greenFretIcon);
                }
            }
            s_textures[tex] = std::move(data);
        }

        struct SyncTag;
        struct DestroyTag;

        void onSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!s_haveSwizzle)
            {
                for (uint32_t t = 0; t < 2; ++t)
                {
                    for (uint32_t i = 0; i < 64; ++i)
                        s_swizzle8[t][i] = load<uint8_t>(rdram, s_addresses->swizzle8[t] + i);
                    for (uint32_t i = 0; i < 128; ++i)
                        s_swizzle4[t][i] = load<uint8_t>(rdram, s_addresses->swizzle4[t] + i);
                }
                s_haveSwizzle = true;
            }
            const uint32_t tex = GPR_U32(ctx, 4);
            if (load<uint32_t>(rdram, tex + milo::tex::kType) & milo::tex::kTypeNoPixels)
                s_textures.erase(tex);
            else
                store(tex, decode(rdram, tex + milo::tex::kBitmap));
        }

        void onDestroy(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t tex = GPR_U32(ctx, 4);
            s_textures.erase(tex);
            std::erase(s_yellowFretIcons, tex);
        }
    }

    std::shared_ptr<const TextureData> capturedTexture(uint32_t tex)
    {
        const auto found = s_textures.find(tex);
        return found != s_textures.end() ? found->second : nullptr;
    }

    void installTextureCapture(PS2Runtime &runtime, const Addresses &addresses)
    {
        s_addresses = &addresses;
        EntryHook<SyncTag>::install(runtime, addresses.psTexSyncBitmap, onSync);
        EntryHook<DestroyTag>::install(runtime, addresses.psTexDestroy, onDestroy);
    }
}
