#pragma once

// Compiled scripts (.dtb) from any of the discs, read on the host. GH1, GH2
// and 80s write them alike.

#include "milo/milo.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace gh2::dtb
{
    // DataNode types, as DataNode::Load (GH1 0x24e910) reads them.
    enum Type : uint32_t
    {
        kInt = 0x00,
        kFloat = 0x01,
        kVar = 0x02,
        kSymbol = 0x05,
        kUnhandled = 0x06,
        kIfdef = 0x07,
        kElse = 0x08,
        kEndif = 0x09,
        kArray = 0x10,
        kCommand = 0x11,
        kString = 0x12,
        kProperty = 0x13,
        kDefine = 0x20,
        kInclude = 0x21,
        kMerge = 0x22,
        kIfndef = 0x23,
        kAutorun = 0x24,
        kUndef = 0x25,
    };

    struct Node
    {
        Type type = kInt;
        int32_t integer = 0;
        float real = 0.0f;
        std::string text;        // a symbol's, string's, variable's or directive's
        std::vector<Node> nodes; // an array's
    };

    // Macros by name: the nodes a symbol of that name stands for.
    using Macros = std::map<std::string, std::vector<Node>>;

    // A built file's bytes (charsys/gen/charsys.dtb), or none.
    using Files = std::function<std::optional<milo::Bytes>(const std::string &path)>;

    // A script (charsys/charsys.dta, read from its built .dtb) as the game
    // loads it (DataArray::Load, GH1 0x2451a0): conditionals applied,
    // includes spliced in, macros defined into `macros` and spliced in
    // where their symbols stand. Merges are not followed.
    std::optional<Node> read(const std::string &script, Macros &macros, const Files &files);

    // The array in `array` whose first node is `key`, as
    // DataArray::FindArray.
    const Node *find(const Node &array, const std::string &key);
    const Node *find(const Node &array, int32_t key);

    // An int or float as a float.
    std::optional<float> number(const Node &node);

    // A node read back as script text, for the game's DataReadString.
    std::string text(const Node &node);
}
