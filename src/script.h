#pragma once

// The game's script language from the host: native commands the game's
// scripts can call, and our own script text run through the game's parser.

#include "addresses.h"

#include <cstdint>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace gh2::script
{
    // A DataNode: its value and type.
    struct Node
    {
        uint32_t value = 0;
        uint32_t type = 0;
    };
    constexpr uint32_t kInt = 0x0u;
    constexpr uint32_t kFloat = 0x1u;
    constexpr uint32_t kSymbol = 0x5u;
    constexpr uint32_t kArray = 0x10u;

    Node floatNode(float value);

    // One call of a native command: `{name arg1 arg2 ...}`.
    struct Call
    {
        uint8_t *rdram;
        R5900Context *ctx;
        PS2Runtime *runtime;
        uint32_t args; // DataArray*, node 0 the command's name

        int size() const;
        Node arg(int i) const; // evaluated
        std::string symbol(int i) const; // "" unless a symbol
        float number(int i) const;       // an int or a float
        uint32_t object(int i) const;    // Hmx::Object*, by name as script finds it
    };

    using Command = Node (*)(const Call &call);

    // Registered with the game when its data system starts (GHUtl::Init).
    void addCommand(const char *name, Command command);

    // The game's DataReadString: the parsed DataArray, kept, or 0. On the
    // guest thread only.
    uint32_t parse(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text);

    // Parsed with the game's DataReadString, then each top-level command
    // evaluated. On the guest thread only.
    void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text);

    // Run once the game's UI objects exist: at the first screen change after
    // the commands are registered.
    void runWhenUiReady(const char *text);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
