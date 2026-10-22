#pragma once

#include "guest/object.h"
#include "script.h"

#include <cstring>

namespace gh2::guest
{
    // One of a DataArray's nodes: a value and its type.
    struct DataNode : Object
    {
        static constexpr uint32_t kSize = 0x08u;
        using Object::Object;

        uint32_t value() const { return get<uint32_t>(0x00u); }
        uint32_t type() const { return get<uint32_t>(0x04u); }
        // A float, or an int as one.
        float number() const
        {
            return type() == script::kFloat ? get<float>(0x00u) : static_cast<float>(get<int32_t>(0x00u));
        }
        void setFloat(float value) const
        {
            set<float>(0x00u, value);
            set<uint32_t>(0x04u, script::kFloat);
        }
    };

    // DataArray: its nodes (+0) and their count (+8).
    struct DataArray : Object
    {
        using Object::Object;

        int size() const { return address ? get<int16_t>(0x08u) : 0; }
        DataNode node(int i) const
        {
            return DataNode(rdram, get<uint32_t>(0x00u) + static_cast<uint32_t>(i) * DataNode::kSize);
        }

        // The array in it starting on that symbol, or on that int; none
        // when there is no such.
        DataArray find(const char *key) const { return find(key, 0); }
        DataArray find(int number) const { return find(nullptr, number); }

    private:
        DataArray find(const char *key, int number) const
        {
            for (int i = 0; i < size(); ++i)
            {
                if (node(i).type() != script::kArray)
                    continue;
                const DataArray found(rdram, node(i).value());
                if (found.size() <= 0)
                    continue;
                const DataNode first = found.node(0);
                if (key ? first.type() == script::kSymbol &&
                              std::strcmp(reinterpret_cast<const char *>(getMemPtr(rdram, first.value())), key) == 0
                        : first.type() == script::kInt && static_cast<int32_t>(first.value()) == number)
                    return found;
            }
            return DataArray(rdram, 0u);
        }
    };
}
