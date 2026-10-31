#pragma once

// The pieces a custom song's tap is drawn with
// (highway/extended/guitar/tap_gems.h), made in the track's scene as it is
// read: each part a tap may show copied as tap_<name> on a copy of its
// material, a lane's gem on a gem.tex of its own, its coloured body darkened to
// glass and its pale lip and lit rim its lane's colour; every other one its
// material's colour darkened as much. Its HOPO cap banded, its top its colour
// lit, as an open's is.

#include <map>
#include <set>
#include <string>

namespace gh2::guitar
{
    constexpr const char *kTapPrefix = "tap_";
    // A piece's name as its tap_ copy.
    std::string tapped(const std::string &name);

    struct TapPieces
    {
        // The materials drawn solid (the caps' and stars' tops), the stars'
        // a little see-through, an open's gem's, and each tap_ mesh's
        // material.
        std::set<std::string> solid, starGlass, openGlass;
        std::map<std::string, std::string> meshMat;
    };
    const TapPieces &tapPieces();

    // The pieces added to the track's scene as it is read.
    void addTapPieces();
}
