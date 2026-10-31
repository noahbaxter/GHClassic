#pragma once

// MD5 (RFC 1321), as chart sites and Clone Hero name a chart: its chart
// file's bytes' hash in lowercase hex.

#include <string>

namespace gh2::md5
{
    std::string hex(const std::string &bytes);
}
