#pragma once

#include <cstdint>

namespace gh2
{
    // The controller the player holds, as libpad reports a pad: pressed
    // buttons active high in libpad's bit order, sticks centred on 0x80.
    struct HostPad
    {
        uint16_t pressed = 0;
        uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    };

    // libpad's button bits.
    namespace pad
    {
        constexpr uint16_t kSelect = 1u << 0, kL3 = 1u << 1, kR3 = 1u << 2, kStart = 1u << 3;
        constexpr uint16_t kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6, kLeft = 1u << 7;
        constexpr uint16_t kL2 = 1u << 8, kR2 = 1u << 9, kL1 = 1u << 10, kR1 = 1u << 11;
        constexpr uint16_t kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15;
    }

    // The frontend's side, on the thread that pumps SDL events. Poll after
    // each pump; `devicesChanged` when a joystick came or went.
    void openInput();
    void pollInput(bool devicesChanged);
    void closeInput();

    // The state the last poll saw. Callable from any thread.
    HostPad hostPad();
}
