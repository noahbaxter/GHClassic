#pragma once

// 360 GH2's song audio, a .mogg (Ogg Vorbis behind a small header), served
// as the .vgs the game streams: made as it is read, never stored.

#include <cstddef>
#include <string>

namespace gh2::mogg
{
    // `vgs` (songs/x/x.vgs) made from `mogg` on that disc.
    void serveAsVgs(const std::string &vgs, size_t disc, const std::string &mogg);
}
