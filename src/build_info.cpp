#include "build_info.h"

#include <regex>

namespace gh2::build
{
    const char *const kVersion = GHC_VERSION;
    const bool kExperimental = GHC_EXPERIMENTAL;

    std::string shownVersion()
    {
        // git describe adds -<commits>-g<commit> past a tag and -dirty with
        // changes; either is a build of the tag's tree moved on. With no tag
        // it is the bare commit, which shows as dev.
        static const std::regex past("(-[0-9]+-g[0-9a-f]+)?(-dirty)?$");
        const std::string version = kVersion;
        const std::string tag = std::regex_replace(version, past, "");
        if (tag[0] != 'v')
            return GHC_BUILT + std::string("  dev");
        return tag == version ? version : GHC_BUILT + std::string("  ") + tag;
    }
}
