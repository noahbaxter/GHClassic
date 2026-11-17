#include "host/boot_picture.h"

#include "disc/disc.h"
#include "save/save.h"
#include "settings/settings.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace gh2::boot_picture
{
    namespace
    {
        constexpr char kMagic[8] = {'G', 'H', 'C', 'B', 'O', 'O', 'T', '1'};

        // What the picture depends on, as one line.
        const std::string &key()
        {
            static const std::string made = []
            {
                std::string out = save::lastGame() + (settings::get(settings::kWidescreen) ? " wide" : " narrow") +
                                  (settings::get(settings::kFastBoot) ? " fast" : " slow");
                for (const auto &image : disc::images())
                    out += " " + image.filename().string();
                return out;
            }();
            return made;
        }

        std::string path()
        {
            return (std::filesystem::path(settings::userDataPath("cache")) / "boot_picture.bin").string();
        }
    }

    std::optional<Picture> kept()
    {
        std::ifstream in(path(), std::ios::binary);
        char magic[8] = {};
        uint32_t keyLength = 0;
        if (!in.read(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof magic) != 0 ||
            !in.read(reinterpret_cast<char *>(&keyLength), sizeof keyLength) || keyLength > 4096u)
            return std::nullopt;
        std::string was(keyLength, '\0');
        Picture picture;
        if (!in.read(was.data(), keyLength) || was != key() ||
            !in.read(reinterpret_cast<char *>(&picture.width), sizeof picture.width) ||
            !in.read(reinterpret_cast<char *>(&picture.height), sizeof picture.height) ||
            !in.read(reinterpret_cast<char *>(&picture.aspect), sizeof picture.aspect) || picture.width == 0u ||
            picture.height == 0u || picture.width > 8192u || picture.height > 8192u)
            return std::nullopt;
        picture.rgba.resize(size_t{picture.width} * picture.height * 4u);
        if (!in.read(reinterpret_cast<char *>(picture.rgba.data()), static_cast<std::streamsize>(picture.rgba.size())))
            return std::nullopt;
        return picture;
    }

    void keep(Picture picture)
    {
        // A thread of its own: the worker's queue holds the boot's font bake
        // for seconds, and a run quit before then never kept it.
        std::thread([picture = std::move(picture), key = key(), to = path()]
                    {
                        const std::string part = to + ".part";
                        std::error_code error;
                        std::filesystem::create_directories(std::filesystem::path(to).parent_path(), error);
                        {
                            std::ofstream out(part, std::ios::binary | std::ios::trunc);
                            const uint32_t keyLength = static_cast<uint32_t>(key.size());
                            out.write(kMagic, sizeof kMagic);
                            out.write(reinterpret_cast<const char *>(&keyLength), sizeof keyLength);
                            out.write(key.data(), keyLength);
                            out.write(reinterpret_cast<const char *>(&picture.width), sizeof picture.width);
                            out.write(reinterpret_cast<const char *>(&picture.height), sizeof picture.height);
                            out.write(reinterpret_cast<const char *>(&picture.aspect), sizeof picture.aspect);
                            out.write(reinterpret_cast<const char *>(picture.rgba.data()),
                                      static_cast<std::streamsize>(picture.rgba.size()));
                            if (!out)
                            {
                                std::cerr << "[boot] could not keep its picture in " << to << std::endl;
                                return;
                            }
                        }
                        std::filesystem::rename(part, to, error);
                    }).detach();
    }
}
