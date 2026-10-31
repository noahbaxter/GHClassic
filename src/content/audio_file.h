#pragma once

// An audio file as content/vgs.h's audio, whichever of Ogg Vorbis, Ogg Opus,
// MP3 or WAV its bytes are, told by its bytes and not its name
// (content/audio_kind.h).

#include "content/vgs.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gh2::audio_file
{
    // Those bytes, decoded as they are read; `name` for the log. None if
    // they are none of them.
    std::unique_ptr<vgs::Pcm> open(std::shared_ptr<const std::vector<uint8_t>> bytes, const std::string &name);
}
