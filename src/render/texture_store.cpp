#include "render/texture_store.h"

#include "disc/ark.h"
#include "formats/bitmap.h"
#include "formats/dxt.h"
#include "milo/layout.h"
#include "milo/milo.h"
#include "milo/objects.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace gh2::textures
{
    namespace
    {
        // What a stand-in's pixel bytes start with, its payload's id after.
        constexpr uint32_t kMagic = 0x58544847u; // "GHTX"
        // How far apart, of 255 and on average, two sizes of one picture
        // can be (samePicture): compression leaves them a few levels off.
        constexpr uint64_t kSamePictureLevels = 12u;

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

        // The bytes a 360 bitmap at `data` takes, header and levels, or 0
        // when it is none.
        size_t bitmapBytes(const uint8_t *data, size_t size)
        {
            const auto header = bitmap::read(data, size);
            const auto encoded = header ? format(header->encoding) : std::nullopt;
            if (!encoded || header->width == 0u || header->height == 0u)
                return 0u;
            size_t bytes = bitmap::kHeaderBytes;
            const uint32_t width = header->width, height = header->height;
            for (uint32_t level = 0; level <= header->mips; ++level)
                bytes += dxt::levelBytes(*encoded, std::max(width >> level, 1u), std::max(height >> level, 1u));
            return bytes <= size ? bytes : 0u;
        }

        // A CubeTex: its six faces' source names, then the six faces as 360
        // bitmaps, square and alike, in D3D's order (+x -x +y -y +z -z:
        // right, left, top, bottom, front, back). The faces are found as the
        // first run of six such bitmaps that ends the body.
        std::shared_ptr<TextureData> decodeCube(const milo::Bytes &body)
        {
            for (size_t at = 0; at + bitmap::kHeaderBytes <= body.size(); ++at)
            {
                const size_t face = bitmapBytes(body.data() + at, body.size() - at);
                if (face == 0u || at + 6u * face != body.size())
                    continue;
                auto out = std::make_shared<TextureData>();
                out->cube = true;
                for (size_t f = 0; f < 6u; ++f)
                {
                    const auto one = decode(body.data() + at + f * face, face);
                    if (!one || one->width != one->height ||
                        (f != 0u && (one->width != out->width || one->mips.size() != out->mips.size())))
                        return nullptr;
                    out->width = one->width;
                    out->height = one->height;
                    out->rgba.insert(out->rgba.end(), one->rgba.begin(), one->rgba.end());
                    out->mips.resize(one->mips.size());
                    for (size_t level = 0; level < one->mips.size(); ++level)
                        out->mips[level].insert(out->mips[level].end(), one->mips[level].begin(),
                                                one->mips[level].end());
                }
                return out;
            }
            return nullptr;
        }

        // A path's file name without its folders or what follows its first
        // dot, in lower case.
        std::string stem(const std::string &path)
        {
            const size_t slash = path.find_last_of("/\\");
            std::string name = path.substr(slash == std::string::npos ? 0u : slash + 1u);
            name = name.substr(0u, name.find('.'));
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            return name;
        }

        // A disc's loose picture files of that name, wherever they are.
        const std::vector<std::string> &picturesNamed(size_t disc, const std::string &name)
        {
            static std::unordered_map<size_t, std::unordered_map<std::string, std::vector<std::string>>> byDisc;
            static const std::vector<std::string> kNone;
            auto found = byDisc.find(disc);
            if (found == byDisc.end())
            {
                found = byDisc.emplace(disc, std::unordered_map<std::string, std::vector<std::string>>{}).first;
                for (const std::string &path : ark::paths(disc, ""))
                    if (path.ends_with(".bmp_xbox") || path.ends_with(".png_xbox"))
                        found->second[stem(path)].push_back(path);
            }
            const auto named = found->second.find(name);
            return named != found->second.end() ? named->second : kNone;
        }

        // Whether `larger`, a whole number of times `own` each way, is the
        // same picture: each of its blocks' mean within a few levels of the
        // pixel of `own` it covers, on the whole.
        bool samePicture(const TextureData &larger, const TextureData &own)
        {
            const uint32_t times = larger.width / own.width;
            if (times < 2u || larger.width != own.width * times || larger.height != own.height * times ||
                larger.rgba.size() != size_t(larger.width) * larger.height * 4u ||
                own.rgba.size() != size_t(own.width) * own.height * 4u)
                return false;
            uint64_t apart = 0u;
            for (uint32_t y = 0; y < own.height; ++y)
                for (uint32_t x = 0; x < own.width; ++x)
                    for (uint32_t c = 0; c < 4u; ++c)
                    {
                        uint32_t sum = 0u;
                        for (uint32_t v = 0; v < times; ++v)
                            for (uint32_t u = 0; u < times; ++u)
                                sum += larger.rgba[(size_t(y * times + v) * larger.width + x * times + u) * 4u + c];
                        const int32_t mean = static_cast<int32_t>(sum / (times * times));
                        apart += static_cast<uint32_t>(std::abs(mean - int32_t(own.rgba[(size_t(y) * own.width + x) * 4u + c])));
                    }
            return apart < uint64_t(own.width) * own.height * 4u * kSamePictureLevels;
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
            {
                const std::pair<std::string, std::string> cube("CubeTex", source.tex);
                const auto at = std::find(dir.entries.begin(), dir.entries.end(), cube);
                return at != dir.entries.end() ? decodeCube(dir.bodies[static_cast<size_t>(at - dir.entries.begin())])
                                               : nullptr;
            }
            const milo::Bytes &body = dir.bodies[static_cast<size_t>(found - dir.entries.begin())];
            const auto tex = milo::readTex(body, dir.version);
            auto own = tex ? decode(body.data() + tex->bitmap, body.size() - tex->bitmap) : nullptr;
            if (!own || tex->source.empty() || dir.version != 25u)
                return own;
            // The largest copy of its picture the disc has. The scene's own
            // is the size of the file it names, but some 50 have a larger
            // one of the same name in another folder (the store's cassette,
            // the rock meter's glow, speaker cones), which no scene of the
            // 360's draws: taken when it is the same picture.
            std::shared_ptr<TextureData> best = own;
            for (const std::string &path : picturesNamed(source.disc, stem(tex->source)))
            {
                const auto file = ark::readFile(source.disc, path);
                const auto larger = file ? decode(file->data(), file->size()) : nullptr;
                if (larger && larger->width > best->width && samePicture(*larger, *own))
                    best = larger;
            }
            return best;
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
