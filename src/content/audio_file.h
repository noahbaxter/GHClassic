#pragma once

// An audio file as content/vgs.h's audio, whichever of Ogg Vorbis, Ogg Opus,
// MP3 or WAV its bytes are, told by its bytes and not its name
// (content/audio_kind.h).

#include "content/file_bytes.h"
#include "content/vgs.h"

#include <memory>
#include <string>

namespace gh2::audio_file
{
    // Those bytes, decoded as they are read; `name` for the log. None if
    // they are none of them.
    std::unique_ptr<vgs::Pcm> open(FileBytes bytes, const std::string &name);
}
