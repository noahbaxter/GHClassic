#pragma once

// A .vgs the game streams, made as it is read from audio that is not one:
// a 360 mogg (content/mogg.h), a custom song's stems (custom/audio.h).

#include "disc/ark.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gh2::vgs
{
    // Audio decoded a block at a time, every channel at one rate.
    class Pcm
    {
    public:
        virtual ~Pcm() = default;

        uint32_t channels = 0u, rate = 0u;
        uint64_t frames = 0u;
        // Its front right, for a mix to stereo: Vorbis puts the centre
        // between the two fronts of 3, 5 or more.
        uint32_t right = 1u;

        // From that frame on.
        virtual bool seek(uint64_t frame) = 0;
        // Up to `most` frames into out[channel], how many it gave; 0 at the end.
        virtual uint32_t read(float *const *out, uint32_t most) = 0;

        // As read, on until `most` or the end.
        uint32_t fill(float *const *out, uint32_t most)
        {
            std::vector<float *> to(out, out + channels);
            uint32_t got = 0u;
            while (got < most)
            {
                const uint32_t n = read(to.data(), most - got);
                if (n == 0u)
                    break;
                got += n;
                for (float *&c : to)
                    c += n;
            }
            return got;
        }
    };

    // `path` served as a .vgs of what `open` gives, opened when the game
    // asks for it. None if it gives nothing.
    void serve(const std::string &path, std::function<std::unique_ptr<Pcm>()> open);
    // That file made now, for a path found some other way (ark::addMaker).
    std::optional<ark::Made> made(const std::string &path, std::function<std::unique_ptr<Pcm>()> open);
    // `path`'s open decoder dropped, so its next read opens it again.
    void reopen(const std::string &path);
}
