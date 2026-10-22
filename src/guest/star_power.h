#pragma once

#include "guest/object.h"

namespace gh2::guest
{
    // StarPower, as Player makes it.
    struct StarPower : Object
    {
        enum State : int32_t
        {
            kNone,
            kPhraseMissed,
            kInPhrase,
            kCaptured,
        };

        using Object::Object;

        bool enabled() const { return get<uint32_t>(0x28u) != 0u; }
        bool active() const { return get<uint32_t>(0x2cu) != 0u; } // star power on
        uint32_t track() const { return get<uint32_t>(0x48u); }
        int32_t state() const { return get<int32_t>(0x58u); }
        void setState(State state) const { set<int32_t>(0x58u, state); }
    };
}
