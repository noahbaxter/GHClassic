#pragma once

// The song list scenes setlists show in (content/setlists.h): GH2's own,
// with paper as long as the list and the custom list's printout, and a copy
// of it whose rows have room for two lines.

#include "milo/milo.h"

#include <cstdint>
#include <optional>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace gh2::setlist_look
{
    constexpr const char *kOwnScene = "ui/gen/sel_song_quickplay.milo_ps2"; // as the disc keeps it

    // A song list scene served at `path`, under a game's folder or GH2's.
    void addScene(const std::string &path, milo::Dir scene);

    // GH2's song list scene for live setlist `name`, its rows two lines, at
    // `look`'s folder's sel_song_<name>.milo: its list of a type of its own,
    // song_<name>, the scene's own type but for its resource,
    // list_song_<name>.milo, whose name and blurb Texts are a title line and
    // a smaller line under it, and the blurb's colours (ui_objects.dta's
    // UIList types, the seventh) the name's in a lighter grey, blue where
    // the name is. GH2's own setlists keep GH2's type. The look, or none if
    // it cannot be made.
    std::optional<std::string> addTwoLineLook(const std::string &name, const std::string &look);

    // A two-line row's text for TextSlot element `element` (0 its tier's
    // header, 1 the song's name, 6 the line under it) in Text `text`: in
    // capitals and cut where it would run under the printout.
    std::string rowText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t text, const std::string &line,
                        uint32_t element);
}
