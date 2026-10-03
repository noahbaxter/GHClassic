#pragma once

#include <cstdint>
#include <cstring>

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

namespace gh2
{
    // Guest memory by guest address, RDRAM or scratchpad alike. Copies rather
    // than casts, since guest addresses carry no host alignment guarantee.
    template <typename T>
    T load(uint8_t *rdram, uint32_t address)
    {
        T value;
        std::memcpy(&value, getMemPtr(rdram, address), sizeof(T));
        return value;
    }

    template <typename T>
    void store(uint8_t *rdram, uint32_t address, T value)
    {
        std::memcpy(getMemPtr(rdram, address), &value, sizeof(T));
    }

    // An STLport vector's push_back as the game inlines it: in place while
    // there is room, else through that vector type's _M_insert_overflow(_aux),
    // which takes the value and a type tag by reference. `scratch` is 16 free
    // guest bytes for them.
    inline void pushBack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t vector, const void *value,
                         uint32_t size, uint32_t insertOverflow, uint32_t scratch)
    {
        const uint32_t finish = load<uint32_t>(rdram, vector + 4u);
        if (finish != load<uint32_t>(rdram, vector + 0xcu))
        {
            std::memcpy(getMemPtr(rdram, finish), value, size);
            store<uint32_t>(rdram, vector + 4u, finish + size);
            return;
        }
        std::memcpy(getMemPtr(rdram, scratch), value, size);
        store<uint8_t>(rdram, scratch + 8u, 0u);
        runtime->callGuestFunction(rdram, ctx, insertOverflow, {vector, finish, scratch, scratch + 8u, 1u, 1u});
    }
}
