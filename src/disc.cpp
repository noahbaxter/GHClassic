#include "disc.h"

#include "settings.h"
#include "sha1.h"

#include "runtime/ps2_disc_image.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <memory>

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
        if (!disc)
            return {};
        const std::string boot = bootPath(readFile(*disc, "SYSTEM.CNF"));
        if (boot.empty())
        {
            why = "not a PS2 game disc";
            return {};
        }
        std::vector<uint8_t> elf = readFile(*disc, boot);
        const std::string sha1 = sha1Hex(elf.data(), elf.size());
        if (sha1 != GHC_RETAIL_SHA1)
        {
            why = boot + " is not Guitar Hero II (USA) as this build knows it (SHA-1 " + sha1 + ")";
            return {};
        }
        return elf;
    }

    std::filesystem::path find(std::string &why)
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
        std::sort(images.begin(), images.end());
        why.clear();
        for (const std::filesystem::path &image : images)
        {
            std::string rejected;
            if (!bootElf(image, rejected).empty())
                return image;
            why += image.filename().string() + ": " + rejected + "\n";
        }
        return {};
    }
}
