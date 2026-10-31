#include "custom/song_ini.h"

#include "custom/text.h"
#include "settings/ini.h"

#include <cstdlib>
#include <sstream>

namespace gh2::custom
{
    Tags parseIni(const std::string &text)
    {
        Tags tags;
        std::istringstream in(ini::withoutBom(text));
        std::string line, section;
        while (std::getline(in, line))
        {
            line = ini::trim(line);
            if (ini::skipped(line))
                continue;
            if (const auto name = ini::sectionName(line))
            {
                section = lower(ini::trim(*name));
                continue;
            }
            const size_t eq = line.find('=');
            if (section != "song" || eq == std::string::npos)
                continue;
            tags[lower(ini::trim(line.substr(0, eq)))] = ini::trim(line.substr(eq + 1u));
        }
        return tags;
    }

    std::optional<std::string> tag(const Tags &tags, const std::string &key)
    {
        const auto it = tags.find(key);
        if (it == tags.end() || it->second.empty())
            return std::nullopt;
        return it->second;
    }

    std::optional<long> number(const Tags &tags, const std::string &key)
    {
        const auto value = tag(tags, key);
        if (!value)
            return std::nullopt;
        char *end = nullptr;
        const long n = std::strtol(value->c_str(), &end, 10);
        return end != value->c_str() ? std::optional<long>(n) : std::nullopt;
    }

    Options options(const Tags &tags)
    {
        Options out;
        if (const auto n = number(tags, "sustain_cutoff_threshold"); n && *n >= 0)
            out.sustainCutoff = static_cast<uint32_t>(*n);
        if (const auto n = number(tags, "multiplier_note"))
            out.starPowerOn103 = *n == 103;
        return out;
    }
}
