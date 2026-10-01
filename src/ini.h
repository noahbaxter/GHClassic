#pragma once

// What settings.ini and input.ini share: whole-line comments, [section]
// headers, and a write that cannot lose the old file.

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace gh2::ini
{
    inline std::string trim(const std::string &s)
    {
        const size_t begin = s.find_first_not_of(" \t\r");
        if (begin == std::string::npos)
            return "";
        return s.substr(begin, s.find_last_not_of(" \t\r") - begin + 1);
    }

    // Blank, or a comment: ';' and '#' only start one at the line's start.
    inline bool skipped(const std::string &trimmed)
    {
        return trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#';
    }

    // "[device "name"]" -> device "name"; the name may hold spaces.
    inline std::optional<std::string> sectionName(const std::string &trimmed)
    {
        if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']')
            return std::nullopt;
        return trimmed.substr(1, trimmed.size() - 2);
    }

    // Written beside the old file and renamed over it, so a failed write
    // leaves the old one whole. Empty on success.
    template <typename Write>
    std::error_code writeReplacing(const std::string &path, Write write)
    {
        const std::string temp = path + ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            if (out)
                write(out);
            if (!out)
                return std::make_error_code(std::errc::io_error);
        }
        std::error_code error;
        std::filesystem::rename(temp, path, error);
        return error;
    }
}
