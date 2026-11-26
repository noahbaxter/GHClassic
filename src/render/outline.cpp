#include "render/outline.h"

#include <unordered_map>

namespace gh2::outline
{
    namespace
    {
        std::unordered_map<uint32_t, Border> s_borders; // game thread
    }

    void set(uint32_t tex, const Border &border)
    {
        s_borders[tex] = border;
    }

    void clear(uint32_t tex)
    {
        s_borders.erase(tex);
    }

    void forgetAll()
    {
        s_borders.clear();
    }

    std::optional<Border> of(uint32_t tex)
    {
        const auto found = s_borders.find(tex);
        return found != s_borders.end() ? std::optional(found->second) : std::nullopt;
    }
}
