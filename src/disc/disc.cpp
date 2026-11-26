#include "disc/disc.h"

#include "content/games.h"
#include "settings/settings.h"
#include "disc/sha1.h"
#include "disc/volume.h"

#include "runtime/ps2_disc_image.h"

#include <SDL3/SDL.h>
#ifdef GHC_EUROPE_PATCH
#include <zstd.h>
#endif

#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <utility>

namespace gh2::disc
{
    namespace
    {
        std::vector<uint8_t> readFile(DiscImage &image, const std::string &path)
        {
            DiscImage::Extent extent;
            if (!image.find(path, extent) || extent.isDir)
                return {};
            std::vector<uint8_t> data(extent.size);
            data.resize(image.readExtent(extent, 0, data.data(), data.size()));
            return data;
        }

        // SYSTEM.CNF's "BOOT2 = cdrom0:\SLUS_214.47;1".
        std::string bootPath(const std::vector<uint8_t> &cnf)
        {
            const std::string text(cnf.begin(), cnf.end());
            const size_t key = text.find("BOOT2");
            if (key == std::string::npos)
                return {};
            const size_t start = text.find("cdrom0:", key);
            const size_t end = text.find_first_of("\r\n", start);
            return start == std::string::npos ? std::string() : text.substr(start, end - start);
        }

        // The supported game a disc is of (games::of), else "".
        std::string gameOf(const std::filesystem::path &image)
        {
            std::string error;
            const std::unique_ptr<Volume> volume = Volume::open(image, error);
            return volume ? games::of(volume->serial()) : std::string();
        }

        // The games whose discs go beside the game disc, in release order.
        constexpr std::pair<const char *, const char *> kExtras[] = {
            {"gh1", "Guitar Hero"},
            {"gh2x", "Guitar Hero II (Xbox 360)"},
            {"gh80s", "Guitar Hero Encore: Rocks the 80s"},
        };

        const char *extraName(const std::string &game)
        {
            for (const auto &[id, name] : kExtras)
                if (game == id)
                    return name;
            return nullptr;
        }

        constexpr const char *kEuropeElf = "SLES_544.42";
#ifdef GHC_EUROPE_PATCH
        // tools/disc.py patch's, made by the build from both executables.
        constexpr uint8_t kEuropePatchBytes[] = {
#include "europe_patch.inc"
        };
        constexpr const uint8_t *kEuropePatch = kEuropePatchBytes;
#else
        constexpr const uint8_t *kEuropePatch = nullptr;
#endif

        // Europe's executable rebuilt as USA's, the patch's zstd frame
        // decoded with it as the prefix (zstd --patch-from), else empty.
        std::vector<uint8_t> fromEurope(const std::vector<uint8_t> &elf)
        {
#ifdef GHC_EUROPE_PATCH
            const unsigned long long size = ZSTD_getFrameContentSize(kEuropePatch, sizeof kEuropePatchBytes);
            if (size > (64u << 20))
                return {};
            std::vector<uint8_t> usa(size);
            ZSTD_DCtx *context = ZSTD_createDCtx();
            size_t made = ZSTD_DCtx_refPrefix(context, elf.data(), elf.size());
            if (!ZSTD_isError(made))
                made = ZSTD_decompressDCtx(context, usa.data(), usa.size(), kEuropePatch, sizeof kEuropePatchBytes);
            ZSTD_freeDCtx(context);
            if (ZSTD_isError(made) || made != usa.size())
                return {};
            return usa;
#else
            (void)elf;
            return {};
#endif
        }
    }

    bool portable()
    {
        const char *base = SDL_GetBasePath();
        std::error_code error;
        return base && std::filesystem::is_directory(std::filesystem::path(base) / kFolder, error);
    }

    std::filesystem::path folder()
    {
        const std::filesystem::path dir = portable() ? std::filesystem::path(SDL_GetBasePath()) / kFolder
                                                     : std::filesystem::path(settings::userDataPath(kFolder));
        std::error_code error;
        std::filesystem::create_directories(dir, error);
        return dir;
    }

    std::vector<uint8_t> bootElf(const std::filesystem::path &image, std::string &why)
    {
        std::unique_ptr<DiscImage> disc = DiscImage::open(image, why);
        const std::string boot = disc ? bootPath(readFile(*disc, "SYSTEM.CNF")) : std::string();
        const std::string game = gameOf(image);
        if (const char *extra = extraName(game))
        {
            why = std::string(extra) + ", an extra disc, not the game disc";
            return {};
        }
        if (boot.empty())
        {
            if (disc)
                why = "not a PS2 game disc";
            return {};
        }
        std::vector<uint8_t> elf = readFile(*disc, boot);
        const std::string sha1 = sha1Hex(elf.data(), elf.size());
        // Europe's disc boots its own build of the game, which the patch
        // turns into the USA one the code is recompiled from.
        if (sha1 != GHC_RETAIL_SHA1 && boot.find(kEuropeElf) != std::string::npos)
        {
            elf = fromEurope(elf);
            if (elf.empty() || sha1Hex(elf.data(), elf.size()) != GHC_RETAIL_SHA1)
            {
                why = kEuropePatch ? "Guitar Hero II (Europe, Australia), but damaged or modified (SHA-1 " + sha1 + ")"
                                   : "Guitar Hero II (Europe, Australia), which this build can't play";
                return {};
            }
            return elf;
        }
        if (sha1 != GHC_RETAIL_SHA1)
        {
            why = game == "gh2" ? "Guitar Hero II, but damaged or modified (SHA-1 " + sha1 + ")"
                                : "not Guitar Hero II (it boots " +
                                      boot.substr(boot.find('\\') + 1u, boot.find(';') - boot.find('\\') - 1u) + ")";
            return {};
        }
        return elf;
    }

    std::vector<std::filesystem::path> images()
    {
        const std::filesystem::path dir = folder();
        std::vector<std::filesystem::path> images;
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(dir, error))
        {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (entry.is_regular_file(error) && (ext == ".iso" || ext == ".chd" || ext == ".bin"))
                images.push_back(entry.path());
        }
        // settings.ini's [disc] prefer first, then by name.
        const std::string preferred = settings::get(settings::kDiscPrefer) == settings::kIso ? ".iso" : ".chd";
        const auto rank = [&](const std::filesystem::path &image) {
            std::string ext = image.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            return ext != preferred;
        };
        std::sort(images.begin(), images.end(), [&](const std::filesystem::path &a, const std::filesystem::path &b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : a < b;
        });
        return images;
    }

    std::filesystem::path find()
    {
        for (const std::filesystem::path &image : images())
        {
            std::string why;
            if (!bootElf(image, why).empty())
                return image;
            std::cerr << "[disc] " << image.filename().string() << ": " << why << std::endl;
        }
        return {};
    }

    std::string missing()
    {
        std::string text =
            "No Guitar Hero II (PS2) disc found. Please place your disc image (.iso, .chd or .bin) into\n" +
            folder().string() + "\nthen restart GH Classic.";
        std::vector<std::string> found;
        for (const std::filesystem::path &image : images())
            if (const std::string game = gameOf(image); extraName(game))
                found.push_back(game);
        if (found.empty())
            return text;
        text += "\n\nContent discs found:";
        for (const auto &[id, name] : kExtras)
            if (std::find(found.begin(), found.end(), id) != found.end())
                text += std::string("\n") + name;
        return text;
    }
}
