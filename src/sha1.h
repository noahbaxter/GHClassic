#pragma once

// SHA-1 (FIPS 180-4), for telling the disc's executable apart from other
// revisions. Not for anything secret.

#include <cstddef>
#include <cstdint>
#include <string>

namespace gh2
{
    // Lowercase hex, 40 characters.
    std::string sha1Hex(const uint8_t *data, size_t size);
}
