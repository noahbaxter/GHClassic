#pragma once

// The controllers SDL sees, open in the device list (device.h), each with the
// layout it plays: its built-in one (profiles.cpp) with its section of
// input.ini laid over it. The host thread's own.

#include "host/device.h"

namespace gh2::input
{
    // SDL reports every joystick already plugged in as added, so the first
    // pump would reopen the ones just opened; only a changed set reopens.
    void openDevices();
    void closeDevices();
    // Each one's raw controls into its snapshot.
    void readSdlDevices();
}
