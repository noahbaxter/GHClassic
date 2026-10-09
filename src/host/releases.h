#pragma once

#include <string>

namespace gh2::update
{
    // The newest release in GitHub's list of them (its releases API's reply)
    // that is newer than `ours` and on its track: v0.6 over v0.5, v0.6-exp.2
    // over v0.6-exp.1 or v0.5-exp.9, neither over the other track's. Empty
    // when there is none, or `ours` is no release's tag (an untagged tree's
    // v0.5-32-gadba86c).
    std::string newerRelease(const std::string &json, const std::string &ours);

    // Whether a version is a release's tag: v0.6, v0.6-exp.2.
    bool isRelease(const std::string &version);
}
