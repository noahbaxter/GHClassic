#pragma once

// An Ogg Vorbis stream as content/vgs.h's audio.

#include "content/vgs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gh2::vorbis
{
    // Those bytes from `from` on, decoded as they are read; `name` for the
    // log. None if they are not Vorbis.
    std::unique_ptr<vgs::Pcm> open(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string &name,
                                   size_t from = 0u);
}
