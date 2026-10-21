#pragma once

#include "guest/object.h"

namespace gh2::guest
{
    // A star phrase: its start tick (+0) and end tick.
    struct StarPhrase : Object
    {
        static constexpr uint32_t kSize = 0x08u;
        using Object::Object;

        int32_t end() const { return get<int32_t>(0x04u); }
    };

    // A track's star phrases, and its gems' phrase flags at +0x20, a byte a
    // gem.
    struct SongDbTrack : Object
    {
        enum Flag : uint8_t
        {
            kFirst = 1u,
            kIn = 2u,
            kLast = 4u,
        };

        using Object::Object;

        Vector<StarPhrase> phrases() const { return member<Vector<StarPhrase>>(0x00u); }
        uint8_t flags(int gem) const { return load<uint8_t>(rdram, get<uint32_t>(0x20u) + static_cast<uint32_t>(gem)); }
        void setFlags(int gem, uint8_t flags) const
        {
            store<uint8_t>(rdram, get<uint32_t>(0x20u) + static_cast<uint32_t>(gem), flags);
        }
    };

    // SongDB: each track's 0x30 bytes in the array at +8.
    struct SongDb : Object
    {
        using Object::Object;

        SongDbTrack track(uint32_t i) const { return SongDbTrack(rdram, get<uint32_t>(0x08u) + i * 0x30u); }
    };
}
