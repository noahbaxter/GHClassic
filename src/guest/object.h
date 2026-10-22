#pragma once

// Views over GH2's own objects in guest memory. Each names its class's
// fields once, at their offsets as its retail ctor lays them out, and reads
// and writes them through load and store. A view holds only the object's
// address, so making one costs nothing.

#include "guest.h"

#include <cstdint>

namespace gh2::guest
{
    struct Object
    {
        uint8_t *rdram;
        uint32_t address;

        Object(uint8_t *r, uint32_t a) : rdram(r), address(a) {}

        // A virtual through the vtable at `vptr`, as GCC 2.95 lays it out:
        // each slot 8 bytes, the this adjustment (an int16), then the function.
        struct Virtual
        {
            uint32_t self, function;
        };
        Virtual virtualAt(uint32_t vptr, uint32_t slot) const
        {
            const uint32_t vtable = get<uint32_t>(vptr);
            return {address + static_cast<uint32_t>(static_cast<int32_t>(load<int16_t>(rdram, vtable + slot))),
                    load<uint32_t>(rdram, vtable + slot + 4u)};
        }

    protected:
        template <typename T>
        T get(uint32_t offset) const
        {
            return load<T>(rdram, address + offset);
        }

        template <typename T>
        void set(uint32_t offset, T value) const
        {
            store<T>(rdram, address + offset, value);
        }

        // The object held at that offset, or pointed to from it.
        template <typename View>
        View member(uint32_t offset) const
        {
            return View(rdram, address + offset);
        }

        template <typename View>
        View pointee(uint32_t offset) const
        {
            return View(rdram, get<uint32_t>(offset));
        }
    };

    // An STLport vector: its start (+0) and finish (+4), Element::kSize
    // bytes an element.
    template <typename Element>
    struct Vector : Object
    {
        using Object::Object;

        int size() const { return static_cast<int>((get<uint32_t>(0x04u) - get<uint32_t>(0x00u)) / Element::kSize); }

        Element operator[](int i) const
        {
            return Element(rdram, get<uint32_t>(0x00u) + static_cast<uint32_t>(i) * Element::kSize);
        }
    };
}
