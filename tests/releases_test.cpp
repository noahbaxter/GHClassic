// Which release the game offers as an update, from GitHub's list of them.
// tools/ghc.py ci builds and runs it; it exits 1 on any failed check.

#include "host/releases.h"

#include <iostream>
#include <string>

namespace
{
    using gh2::update::isRelease;
    using gh2::update::newerRelease;

    int s_failed = 0;

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::cerr << "failed: " #condition << " (line " << __LINE__ << ")" << std::endl; \
            ++s_failed; \
        } \
    } while (0)

    // As the API prints it: newest first, other keys between.
    const std::string kList = R"([
  { "url": "https://api.github.com/repos/x/y/releases/3", "tag_name": "v0.7-exp.2", "draft": false, "prerelease": true },
  { "url": "https://api.github.com/repos/x/y/releases/2", "tag_name": "v0.10", "draft": false, "prerelease": true },
  { "url": "https://api.github.com/repos/x/y/releases/1", "tag_name": "v0.7-exp.1", "name": "v0.9 notes" },
  { "tag_name":"v0.6" },
  { "tag_name": "nightly" }
])";
}

int main()
{
    // Its own track's newest, numbers compared as numbers.
    CHECK(newerRelease(kList, "v0.5") == "v0.10");
    CHECK(newerRelease(kList, "v0.6") == "v0.10");
    CHECK(newerRelease(kList, "v0.6.1") == "v0.10");
    CHECK(newerRelease(kList, "v0.10").empty());
    CHECK(newerRelease(kList, "v1.0").empty());
    CHECK(newerRelease(kList, "v0.6-exp.4") == "v0.7-exp.2");
    CHECK(newerRelease(kList, "v0.7-exp.1") == "v0.7-exp.2");
    CHECK(newerRelease(kList, "v0.7-exp.2").empty());
    CHECK(newerRelease(kList, "v0.8-exp.1").empty());

    // No release: an untagged tree, a build with no version, an empty or
    // broken reply.
    CHECK(newerRelease(kList, "v0.5-32-gadba86c").empty());
    CHECK(newerRelease(kList, "v0.5-dirty").empty());
    CHECK(newerRelease(kList, "dev").empty());
    CHECK(newerRelease("[]", "v0.5").empty());
    CHECK(newerRelease("", "v0.5").empty());
    CHECK(newerRelease("{ \"tag_name\": \"v0.9", "v0.5").empty());

    CHECK(isRelease("v0.6"));
    CHECK(isRelease("v0.6-exp.2"));
    CHECK(!isRelease("v0.6-exp"));
    CHECK(!isRelease("v0.6."));
    CHECK(!isRelease("0.6"));
    CHECK(!isRelease("v0.5-32-gadba86c"));

    if (s_failed)
        std::cerr << s_failed << " failed" << std::endl;
    return s_failed ? 1 : 0;
}
