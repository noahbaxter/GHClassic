#pragma once

// An Ogg Vorbis stream as content/vgs.h's audio.

#include "content/file_bytes.h"
#include "content/vgs.h"

#include <memory>
#include <string>

namespace gh2::vorbis
{
    // Those bytes, decoded as they are read; `name` for the log. None if
    // they are not Vorbis.
    std::unique_ptr<vgs::Pcm> open(FileBytes bytes, const std::string &name);
}
