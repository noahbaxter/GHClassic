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
    constexpr uint32_t kCommand = 0x11u;  // {...}
    constexpr uint32_t kProperty = 0x13u; // [...]

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

    // A copy of `array` and of every array, command and property under it,
    // each with its file and line, holding one reference. Strings are
    // shared. DataArray::Clone (0x2b0558) copies arrays alone and no file.
    uint32_t clone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t array);

    // A copy of `array`, of type `type`, and of all under it, in the
    // runtime's memory above the game's heap (PS2Runtime::guestMalloc): one
    // to keep for good at no cost to the game's 32 MB. It holds a reference
    // that is never let go, so the game frees none of it, and is only for
    // clone to read.
    uint32_t park(uint8_t *rdram, PS2Runtime *runtime, uint32_t array, uint32_t type = kArray);

    // As a DataNode lets go of its array (DataNode::~DataNode, 0x2b8110).
    void release(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t array);

    // Parsed with the game's DataReadString, each top-level command
    // evaluated, then let go: what a run defines holds its own arrays
    // (Hmx::Object::SetTypeDef, 0x2c1360). On the guest thread only.
    void run(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text);

    // The game's Symbol for `text`, interned. On the guest thread only.
    uint32_t symbol(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::string &text);

    // $name = node, written raw: no reference taken or released, so an
    // array set here must outlive the variable or be cleared after use.
    void setVariable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *name, Node node);

    // Run once the game's UI objects exist: at the first screen change after
    // the commands are registered.
    void runWhenUiReady(std::string text);

    // The same, for a script that changes the game's screens: run again by
    // patchUiAgain once they have taken another game's scripts
    // (content/campaigns.h), so it must stand being run twice. It is parsed
    // once, and each run is of a copy.
    void patchUi(std::string text);
    void patchUiAgain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    void install(PS2Runtime &runtime, const Addresses &addresses);
}
