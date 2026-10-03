#pragma once

// A disc's files, read in place: a PS2 disc image's ISO 9660 (DiscImage) or
// an Xbox 360 disc's XDVDFS.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

class DiscImage;

namespace gh2
{
    class Volume
    {
    public:
        // Bytes from the volume's start.
        struct File
        {
            uint64_t offset = 0u;
            uint64_t size = 0u;
        };

        virtual ~Volume() = default;

        // A path on the disc, case-insensitive.
        virtual std::optional<File> find(const std::string &path) = 0;
        // Returns how many bytes were read. Safe from any thread.
        virtual size_t read(uint64_t offset, uint8_t *dst, size_t size) = 0;
        // What the disc boots: "SLUS_215.86" from a PS2's SYSTEM.CNF, a
        // 360's title ID as "415607E7".
        virtual std::string serial() = 0;

        // A PS2 or 360 disc image, else null and why.
        static std::unique_ptr<Volume> open(const std::filesystem::path &path, std::string &error);
        // The PS2 disc image the runtime already has open.
        static std::unique_ptr<Volume> of(DiscImage &disc);
    };
}
