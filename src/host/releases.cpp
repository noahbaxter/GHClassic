#include "host/releases.h"

#include <cstdlib>
#include <optional>
#include <vector>

namespace gh2::update
{
    namespace
    {
        // A release's tag: v0.6 is {0, 6}; v0.6-exp.2 the same on the
        // experimental track, its second there.
        struct Version
        {
            std::vector<int> numbers;
            bool experimental = false;
            int build = 0;

            bool operator<(const Version &other) const
            {
                return numbers != other.numbers ? numbers < other.numbers : build < other.build;
            }
        };

        // "0.6" as {0, 6}; empty for anything else.
        std::vector<int> numbers(const std::string &text)
        {
            std::vector<int> out;
            for (size_t at = 0; at < text.size();)
            {
                if (text[at] < '0' || text[at] > '9')
                    return {};
                char *end = nullptr;
                out.push_back(static_cast<int>(std::strtol(text.c_str() + at, &end, 10)));
                at = static_cast<size_t>(end - text.c_str());
                if (at < text.size() && (text[at++] != '.' || at == text.size()))
                    return {};
            }
            return out;
        }

        std::optional<Version> parse(const std::string &tag)
        {
            if (tag.size() < 2u || tag[0] != 'v')
                return std::nullopt;
            Version version;
            const size_t dash = tag.find('-');
            version.numbers = numbers(tag.substr(1u, dash == std::string::npos ? dash : dash - 1u));
            if (version.numbers.empty())
                return std::nullopt;
            if (dash != std::string::npos)
            {
                const std::vector<int> build = tag.compare(dash, 5u, "-exp.") == 0 ? numbers(tag.substr(dash + 5u))
                                                                                    : std::vector<int>();
                if (build.size() != 1u)
                    return std::nullopt;
                version.experimental = true;
                version.build = build[0];
            }
            return version;
        }

        // Each "tag_name" of the reply. Drafts are not in it.
        std::vector<std::string> tags(const std::string &json)
        {
            std::vector<std::string> out;
            const std::string key = "\"tag_name\"";
            for (size_t at = json.find(key); at != std::string::npos; at = json.find(key, at))
            {
                at = json.find('"', json.find(':', at + key.size()));
                const size_t end = at == std::string::npos ? at : json.find('"', at + 1u);
                if (end == std::string::npos)
                    break;
                out.push_back(json.substr(at + 1u, end - at - 1u));
                at = end;
            }
            return out;
        }
    }

    bool isRelease(const std::string &version) { return parse(version).has_value(); }

    std::string newerRelease(const std::string &json, const std::string &ours)
    {
        const auto mine = parse(ours);
        if (!mine)
            return {};
        std::string newest;
        Version best = *mine;
        for (const std::string &tag : tags(json))
        {
            const auto version = parse(tag);
            if (version && version->experimental == mine->experimental && best < *version)
            {
                best = *version;
                newest = tag;
            }
        }
        return newest;
    }
}
