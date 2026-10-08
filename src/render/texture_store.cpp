#include "render/texture_store.h"

#include "disc/ark.h"
#include "formats/bitmap.h"
#include "formats/dxt.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "milo/objects.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace gh2::textures
{
    namespace
    {
        // What a stand-in's pixel bytes start with, its payload's id after.
        constexpr uint32_t kMagic = 0x58544847u; // "GHTX"
        constexpr size_t kNameBytes = 8u;

        // Held only by what draws it: the capture's entry for each guest
        // texture, then the renderer's copy for a few frames more.
        struct Payload
        {
            Source source;
            std::weak_ptr<const TextureData> data;
        };
        std::unordered_map<uint32_t, Payload> s_payloads;

        // The scene file last read from. A screen's textures are first drawn
        // in the same frame, and a file is inflated whole to reach one.
        struct Open
        {
            size_t disc;
            std::string path;
            milo::Dir dir;
        };
        std::optional<Open> s_open;

        bool same(const Source &a, const Source &b)
        {
            return a.disc == b.disc && a.path == b.path && a.tex == b.tex;
        }

        uint32_t hash(const Source &source)
        {
            uint32_t hash = 2166136261u; // FNV-1a
            const auto mix = [&](uint8_t byte) { hash = (hash ^ byte) * 16777619u; };
            mix(static_cast<uint8_t>(source.disc));
            for (const std::string *text : {&source.path, &source.tex})
            {
                for (const char c : *text)
                    mix(static_cast<uint8_t>(c));
                mix(0u);
            }
            return hash;
        }

        std::optional<dxt::Format> format(uint32_t encoding)
        {
            switch (encoding)
            {
            case bitmap::kDxt1:
                return dxt::kDxt1;
            case bitmap::kDxt5:
                return dxt::kDxt5;
            case bitmap::kAti2:
                return dxt::kAti2;
            }
            return std::nullopt;
        }

        // A 360 bitmap: its header, then each level, the next half the one
        // before.
        std::shared_ptr<TextureData> decode(const uint8_t *data, size_t size)
        {
            const auto header = bitmap::read(data, size);
            const auto encoded = header ? format(header->encoding) : std::nullopt;
            if (!encoded || header->width == 0u || header->height == 0u)
                return nullptr;
            auto out = std::make_shared<TextureData>();
            out->width = header->width;
            out->height = header->height;
            data += bitmap::kHeaderBytes;
            size -= bitmap::kHeaderBytes;
            for (uint32_t level = 0; level <= header->mips; ++level)
            {
                const uint32_t width = std::max(out->width >> level, 1u);
                const uint32_t height = std::max(out->height >> level, 1u);
                std::vector<uint8_t> rgba;
                if (!dxt::decode(*encoded, data, size, width, height, rgba))
                    break;
                const size_t used = dxt::levelBytes(*encoded, width, height);
                data += used;
                size -= used;
                if (level == 0u)
                    out->rgba = std::move(rgba);
                else
                    out->mips.push_back(std::move(rgba));
            }
            return out->rgba.empty() ? nullptr : out;
        }

        std::shared_ptr<TextureData> read(const Source &source)
        {
            if (source.tex.empty())
            {
                const auto file = ark::readFile(source.disc, source.path);
                return file ? decode(file->data(), file->size()) : nullptr;
            }
            if (!s_open || s_open->disc != source.disc || s_open->path != source.path)
            {
                s_open.reset();
                const auto file = ark::readFile(source.disc, source.path);
                auto dir = file ? milo::read(*file) : std::nullopt;
                if (!dir)
                    return nullptr;
                s_open = Open{source.disc, source.path, std::move(*dir)};
            }
            const milo::Dir &dir = s_open->dir;
            const std::pair<std::string, std::string> entry("Tex", source.tex);
            const auto found = std::find(dir.entries.begin(), dir.entries.end(), entry);
            if (found == dir.entries.end())
                return nullptr;
            const milo::Bytes &body = dir.bodies[static_cast<size_t>(found - dir.entries.begin())];
            const auto tex = milo::readTex(body, dir.version);
            return tex ? decode(body.data() + tex->bitmap, body.size() - tex->bitmap) : nullptr;
        }
    }

    uint32_t add(const Source &source)
    {
        // Its hash, or the next id free when another source has that.
        uint32_t id = hash(source);
        for (auto found = s_payloads.find(id); found != s_payloads.end(); found = s_payloads.find(++id))
            if (same(found->second.source, source))
                return id;
        s_payloads[id] = {source, {}};
        return id;
    }

    std::vector<uint8_t> standIn(uint32_t id, uint32_t width, uint32_t height)
    {
        bitmap::Header header;
        header.bpp = 4u;
        header.encoding = ::milo::bitmap::kRgba | ::milo::bitmap::kPs2Alpha;
        header.width = static_cast<uint16_t>(width);
        header.height = static_cast<uint16_t>(height);
        header.rowBytes = static_cast<uint16_t>((width + 1u) / 2u);
        const size_t pixels = static_cast<size_t>(header.rowBytes) * height;
        if (width > 0xffffu || height > 0xffffu || pixels < kNameBytes)
            return {};
        std::vector<uint8_t> out;
        bitmap::write(out, header);
        const size_t name = out.size() + bitmap::paletteBytes(header);
        out.resize(name + pixels, 0u);
        std::memcpy(out.data() + name, &kMagic, 4u);
        std::memcpy(out.data() + name + 4u, &id, 4u);
        return out;
    }

    std::optional<uint32_t> named(const uint8_t *pixels, size_t size)
    {
        uint32_t magic = 0u, id = 0u;
        if (size < kNameBytes)
            return std::nullopt;
        std::memcpy(&magic, pixels, 4u);
        std::memcpy(&id, pixels + 4u, 4u);
        return magic == kMagic && s_payloads.contains(id) ? std::optional(id) : std::nullopt;
    }

    std::shared_ptr<const TextureData> get(uint32_t id)
    {
        const auto found = s_payloads.find(id);
        if (found == s_payloads.end())
            return nullptr;
        Payload &payload = found->second;
        if (auto held = payload.data.lock())
            return held;
        std::shared_ptr<const TextureData> data = read(payload.source);
        if (!data)
            std::cerr << "[textures] cannot read " << payload.source.path << " " << payload.source.tex << std::endl;
        payload.data = data;
        return data;
    }

    void endFrame() { s_open.reset(); }
}
