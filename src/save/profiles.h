#pragma once

// Who plays: a profile is a person, apart from GH2's bands (its careers),
// kept in save.bin by number from 1:
//
//   [profiles]
//   version = 1
//   1 = Noah
//   3 = Sam
//
// Its bests go in [profile 1 best] (save/best.h). Each seat plays as the
// profile its controller last played as, kept in state.ini's [profiles] by
// the device, as [seats] names it, as "<number> <name>" so a profile made
// later under a gone one's number is not taken for it, or as a guest. A
// controller with neither is a guest, as is one whose profile a seat before
// it plays as. A save with bands and no profiles yet gets one of each
// band's name, the first taking the bests kept before profiles. Not one
// goes on a PCSX2 card: a card's saves are its bands'.

#include <array>
#include <optional>
#include <string>

namespace gh2::save
{
    // GH2's band list has eight rows, the last a guest's.
    constexpr int kProfiles = 7;

    struct Profiles
    {
        std::array<std::string, kProfiles> names; // each number's, "" for none

        bool here(std::optional<int> profile) const
        {
            return profile && *profile >= 0 && *profile < kProfiles && !names[static_cast<size_t>(*profile)].empty();
        }
        // The first number with no profile, none when all are taken.
        std::optional<int> free() const;
    };
    Profiles profiles();
    void nameProfile(int profile, const std::string &name);

    // The profile seat `seat` plays as, none for a guest; and the one its
    // controller is to play as from now (a profile plays on one controller,
    // so whichever had it last forgets it).
    std::optional<int> seatProfile(int seat);
    void keepSeatProfile(int seat, std::optional<int> profile);
}
