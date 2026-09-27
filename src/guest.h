#pragma once

#include <cstdint>
#include <cstring>

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
}
